/* SPU output on the Wii audio interface.
 *
 * The SPU is clocked by guest time, as on the desktop host: every 768 guest
 * cycles is one 44.1 kHz frame. The runtime calls the pump at each vblank (and
 * per sample while the SPU IRQ is armed); it renders what guest time has
 * produced, resamples it to the interface's 48 kHz and queues it in a ring
 * that the audio DMA interrupt drains. */
#include <stdint.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "spu.h"
#include "wii_host.h"

extern uint64_t psx_get_cycle_count(void);
extern void psx_set_midframe_audio_pump(void (*fn)(void));

#define CYCLES_PER_FRAME 768u           /* 33.8688 MHz / 44100 Hz */
#define DMA_FRAMES       256            /* 5.3 ms per DMA block at 48 kHz */
#define RING_FRAMES      8192           /* power of two */
#define RENDER_MAX       512

static int16_t s_dma[2][DMA_FRAMES * 2] __attribute__((aligned(32)));
static int s_dma_cur;

/* Single producer (guest thread), single consumer (DMA interrupt). */
static uint32_t s_ring[RING_FRAMES];
static volatile uint32_t s_wr, s_rd;
static volatile unsigned s_underruns, s_dropped;
static uint32_t s_last_out;

static uint64_t s_last_cycles, s_carry, s_render_ticks;
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

static inline void push(int16_t l, int16_t r)
{
    uint32_t wr = s_wr;
    if (wr - s_rd >= RING_FRAMES) { s_dropped++; return; }
    s_ring[wr & (RING_FRAMES - 1)] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
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

static void pump(void)
{
    static int16_t buf[RENDER_MAX * 2];
    uint64_t now = psx_get_cycle_count();
    if (s_last_cycles == 0 || now < s_last_cycles) { s_last_cycles = now; return; }
    uint64_t delta = now - s_last_cycles + s_carry;
    s_last_cycles = now;
    uint32_t frames = (uint32_t)(delta / CYCLES_PER_FRAME);
    s_carry = delta % CYCLES_PER_FRAME;
    uint64_t t0 = gettime();
    while (frames) {
        int n = frames > RENDER_MAX ? RENDER_MAX : (int)frames;
        spu_render(buf, n);
        resample(buf, n);
        frames -= (uint32_t)n;
    }
    s_render_ticks += gettime() - t0;
}

uint32_t wii_audio_queued(void)
{
    return s_wr - s_rd;
}

void wii_audio_stats(unsigned *underruns, unsigned *dropped, unsigned *render_ms)
{
    *underruns = s_underruns;
    *dropped = s_dropped;
    *render_ms = (unsigned)ticks_to_millisecs(s_render_ticks);
    s_render_ticks = 0;
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
}
