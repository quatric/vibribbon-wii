/* SPU output on the Wii audio interface.
 *
 * The SPU is clocked by guest time, as on the desktop host: every 768 guest
 * cycles is one 44.1 kHz frame. The runtime calls the pump at each vblank (and,
 * while the SPU IRQ is armed, wherever it could fire); it renders what guest
 * time has produced, resamples it to the interface's 48 kHz and queues it in a
 * ring that the audio DMA interrupt drains. */
#include <stdint.h>
#include <string.h>
#include <gccore.h>

#include "spu.h"
#include "wii_host.h"

extern uint64_t psx_get_cycle_count(void);
extern void psx_set_midframe_audio_pump(void (*fn)(void));
extern void psx_set_spu_sample_due(uint64_t (*fn)(uint32_t ahead));
extern void (*g_spu_access_hook)(void);
extern uint16_t spu_get_ctrl(void);
extern int psx_in_device_service;
extern uint64_t g_spu_render_cycle;
extern uint32_t g_spu_render_step;

#define CYCLES_PER_FRAME 768u           /* 33.8688 MHz / 44100 Hz */
#define DMA_FRAMES       256            /* 5.3 ms per DMA block at 48 kHz */
#define RING_FRAMES      8192           /* power of two */
#define RENDER_MAX       512             /* frames per spu_render call */

static int16_t s_dma[2][DMA_FRAMES * 2] __attribute__((aligned(32)));
static int s_dma_cur;

/* Single producer (guest thread), single consumer (DMA interrupt). */
static uint32_t s_ring[RING_FRAMES];
static volatile uint32_t s_wr, s_rd;
static volatile unsigned s_underruns, s_dropped;
static uint32_t s_last_out;

/* The SPU timeline: sample n is rendered once guest time reaches
 * s_origin + (n + 1) * 768, s_rendered samples so far. */
static uint64_t s_origin, s_rendered;
static int s_started;
static unsigned s_stat_frames;
static uint32_t s_phase;                /* 16.16 position between s_prev and the next frame */
static int16_t s_prev[2];
#define STEP ((uint32_t)((44100ull << 16) / 48000u))

static void dma_done(void)
{
    int16_t *buf = s_dma[s_dma_cur];
    uint32_t rd = s_rd, avail = s_wr - rd;
    uint32_t n = avail < DMA_FRAMES ? avail : DMA_FRAMES;
    uint32_t *out = (uint32_t *)buf;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_ring[(rd + i) & (RING_FRAMES - 1)];
    if (n) s_last_out = out[n - 1];
    if (n < DMA_FRAMES) {
        /* hold the last frame rather than clicking to zero */
        for (uint32_t i = n; i < DMA_FRAMES; i++) out[i] = s_last_out;
        if (s_wr != 0) s_underruns++;
    }
    s_rd = rd + n;
    DCFlushRange(buf, sizeof(s_dma[0]));
    AUDIO_InitDMA((u32)buf, sizeof(s_dma[0]));
    s_dma_cur ^= 1;
}

#ifdef VIB_WAVDUMP
/* Debug aid: record 6 s of output from vblank VIB_WAVDUMP on (see wii_audio_wavdump). */
#define WAV_FRAMES (48000 * 6)
static uint32_t *s_wav;
static unsigned s_wav_n;
static int s_wav_on;
#endif

static inline void push(int16_t l, int16_t r)
{
#ifdef VIB_WAVDUMP
    if (s_wav_on && s_wav_n < WAV_FRAMES)
        s_wav[s_wav_n++] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
#endif
    uint32_t wr = s_wr;
    if (wr - s_rd >= RING_FRAMES) { s_dropped++; return; }
    /* The audio interface reads big-endian pairs, right channel first. */
    s_ring[wr & (RING_FRAMES - 1)] = ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    /* The frame must be in the ring before the DMA interrupt can see it
     * counted: without this the compiler stores s_wr first, and an interrupt
     * between the two stores plays a frame 8192 samples stale. */
    __asm__ volatile("" ::: "memory");
    s_wr = wr + 1;
}

/* Linear 44.1 -> 48 kHz: emit output frames while the phase lies inside
 * [prev, cur), then advance to the next input frame. */
static void resample(const int16_t *in, int frames)
{
    for (int i = 0; i < frames; i++) {
        int16_t l = in[i * 2], r = in[i * 2 + 1];
        while (s_phase < 0x10000u) {
            int32_t f = (int32_t)(s_phase >> 1);   /* 15 bits keeps the product in 32 */
            push((int16_t)(s_prev[0] + (((l - s_prev[0]) * f) >> 15)),
                 (int16_t)(s_prev[1] + (((r - s_prev[1]) * f) >> 15)));
            s_phase += STEP;
        }
        s_phase -= 0x10000u;
        s_prev[0] = l;
        s_prev[1] = r;
    }
}

static void render_to(uint64_t now)
{
    static int16_t buf[RENDER_MAX * 2];
    if (!s_started) { s_origin = now; s_started = 1; return; }
    if (now < s_origin) return;
    uint64_t target = (now - s_origin) / CYCLES_PER_FRAME;
    if (target <= s_rendered) return;
    uint32_t frames = (uint32_t)(target - s_rendered);
    s_rendered = target;
    s_stat_frames += frames;
    g_spu_render_cycle = now;       /* CD input due later stays queued */
    while (frames) {
        int n = frames > RENDER_MAX ? RENDER_MAX : (int)frames;
        spu_render(buf, n);
        resample(buf, n);
        frames -= (uint32_t)n;
    }
}

/* While the SPU IRQ is enabled the runtime used to pump at every sample
 * boundary. It now pumps only where the SPU could raise the IRQ (see
 * sample_due), and the pumps in between are run here, in order, whenever the
 * guest could see their result: before an SPU register or DMA access and
 * before any other pump. Each renders what it always did, so the SPU sees the
 * same calls. s_pumped is the guest time the pumps have covered. */
static uint64_t s_pumped;
static uint32_t s_origin_mod;           /* s_origin % CYCLES_PER_FRAME */

/* The boundary at which the pumps render sample s_rendered + ahead. */
static uint64_t sample_due(uint32_t ahead)
{
    if (!s_started) return UINT64_MAX;
    uint64_t done = s_origin + (s_rendered + ahead + 1u) * CYCLES_PER_FRAME;
    return done + (s_origin_mod ? CYCLES_PER_FRAME - s_origin_mod : 0u);
}

/* The pumps at boundaries b + 768, ..., b + n * 768 after one at b: each
 * renders one sample, so they render as one run, with the SPU stepping the
 * guest time its CD input sees by a sample per frame. */
static void render_run(uint64_t b, uint32_t n)
{
    static int16_t buf[RENDER_MAX * 2];
    s_rendered += n;
    s_stat_frames += n;
    g_spu_render_cycle = b + CYCLES_PER_FRAME;
    g_spu_render_step = CYCLES_PER_FRAME;
    while (n) {
        int k = n > RENDER_MAX ? RENDER_MAX : (int)n;
        spu_render(buf, k);
        resample(buf, k);
        n -= (uint32_t)k;
    }
    g_spu_render_step = 0;
}

static void pump_boundaries(uint64_t t)
{
    if (s_started && (spu_get_ctrl() & 0x0040u)) {
        uint64_t b = sample_due(0);
        uint64_t first = s_pumped + CYCLES_PER_FRAME - (uint32_t)(s_pumped % CYCLES_PER_FRAME);
        if (b < first) b = first;
        if (b <= t) {
            render_to(b);
            if (b >= s_origin)
                render_run(b, (uint32_t)((t - b) / CYCLES_PER_FRAME));
        }
    }
    if (t > s_pumped) s_pumped = t;
}

/* Inside a device pass, the pump for a boundary at the current cycle runs
 * after the devices, so an access from a device comes before it. */
static void spu_access(void)
{
    uint64_t now = psx_get_cycle_count();
    pump_boundaries(psx_in_device_service ? now - 1u : now);
}

/* Called by the runtime at vblank and at the sample events. */
static void pump(void)
{
    uint64_t now = psx_get_cycle_count();
    pump_boundaries(now);
    if (!s_started) s_origin_mod = (uint32_t)(now % CYCLES_PER_FRAME);
    render_to(now);
}

uint32_t wii_audio_queued(void)
{
    return s_wr - s_rd;
}

void wii_audio_stats(unsigned *underruns, unsigned *dropped, unsigned *rendered)
{
    *underruns = s_underruns;
    *dropped = s_dropped;
    *rendered = s_stat_frames;
    s_stat_frames = 0;
}

void wii_audio_init(void)
{
    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(dma_done);
    memset(s_dma, 0, sizeof(s_dma));
    DCFlushRange(s_dma, sizeof(s_dma));
    AUDIO_InitDMA((u32)s_dma[0], sizeof(s_dma[0]));
    AUDIO_StartDMA();
    s_dma_cur = 1;
    psx_set_midframe_audio_pump(pump);
    psx_set_spu_sample_due(sample_due);
    g_spu_access_hook = spu_access;
}

#ifdef VIB_WAVDUMP
#include <stdio.h>
#include <stdlib.h>
/* Called every vblank: start recording at frame VIB_WAVDUMP, write
 * sd:/apps/vibribbon/audio.wav (48 kHz stereo, little-endian) once full. */
void wii_audio_wavdump(unsigned frame)
{
    static int done;
    if (done) return;
    if (!s_wav_on && frame >= VIB_WAVDUMP) {
        s_wav = malloc(WAV_FRAMES * 4);
        s_wav_on = s_wav != NULL;
        if (!s_wav) done = 1;
        return;
    }
    if (!s_wav_on || s_wav_n < WAV_FRAMES) return;
    done = 1;
    s_wav_on = 0;
    uint8_t *b = (uint8_t *)s_wav;
    for (unsigned i = 0; i < WAV_FRAMES; i++) {     /* l,r big-endian -> l,r little-endian */
        uint32_t v = s_wav[i];
        b[i * 4 + 0] = (uint8_t)(v >> 16); b[i * 4 + 1] = (uint8_t)(v >> 24);
        b[i * 4 + 2] = (uint8_t)v;         b[i * 4 + 3] = (uint8_t)(v >> 8);
    }
    static const uint8_t hdr_fmt[] = {
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0 };
    uint32_t data = WAV_FRAMES * 4, riff = data + 36;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    hdr[4] = riff; hdr[5] = riff >> 8; hdr[6] = riff >> 16; hdr[7] = riff >> 24;
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, hdr_fmt, sizeof(hdr_fmt));
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data; hdr[41] = data >> 8; hdr[42] = data >> 16; hdr[43] = data >> 24;
    extern void wii_write_file(const char *path, const void *a, unsigned na, const void *b, unsigned nb);
    wii_write_file("sd:/apps/vibribbon/audio.wav", hdr, sizeof(hdr), b, data);
    free(s_wav);
}
#endif
