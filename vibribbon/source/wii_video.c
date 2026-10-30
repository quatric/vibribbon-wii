/* Video output: the PS1 display rectangle is converted from 15-bit RGB VRAM
 * into the Wii's YCbCr external framebuffer and scaled to the TV. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "wii_host.h"

static void *s_xfb[2];
static int s_fb;
static GXRModeObj *s_rmode;
static uint32_t s_ycbcr_lut[32768];   /* RGB555 -> Y << 16 | Cb << 8 | Cr */

static void build_ycbcr_lut(void)
{
    for (int p = 0; p < 32768; p++) {
        int r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
        int y  = (299 * r + 587 * g + 114 * b) / 1000;
        int cb = 128 + (-169 * r - 331 * g + 500 * b) / 1000;
        int cr = 128 + (500 * r - 419 * g - 81 * b) / 1000;
        if (cb < 0) cb = 0; if (cb > 255) cb = 255;
        if (cr < 0) cr = 0; if (cr > 255) cr = 255;
        s_ycbcr_lut[p] = ((uint32_t)y << 16) | ((uint32_t)cb << 8) | (uint32_t)cr;
    }
}

/* HOME, the console's Reset and Power buttons and a Wii Remote's power button
 * only set a flag (the buttons' callbacks run in interrupt context). The exit
 * itself runs on the main thread once the guest thread has parked: leaving
 * from the guest thread with the Wii Remotes and USB still open hung on the
 * way back to the Homebrew Channel. */
static volatile int s_exit_req;   /* 1 back to the loader, 2 power off */
static volatile int s_guest_parked;
static lwp_t s_main_thread;
static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; s_exit_req = 1; }
static void on_power(void) { s_exit_req = 2; }
static void on_wpad_power(s32 chan) { (void)chan; s_exit_req = 2; }

void wii_video_init(void)
{
    s_main_thread = LWP_GetSelf();
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);
    s_rmode = VIDEO_GetPreferredMode(NULL);
    s_xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
    s_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
    VIDEO_Configure(s_rmode);
    VIDEO_ClearFrameBuffer(s_rmode, s_xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(s_rmode, s_xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(s_xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (s_rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    build_ycbcr_lut();
    CON_InitEx(s_rmode, 20, 20, s_rmode->fbWidth - 40, s_rmode->xfbHeight - 40);
}

/* The log goes to SD from a writer thread below the guest's priority: a write
 * with its fsync takes most of a millisecond, which the guest thread would
 * otherwise spend on every line. Lines wait in a ring until the guest idles. */
static FILE *s_logf;
static mutex_t s_log_lock = LWP_MUTEX_NULL;   /* libfat is not thread-safe */
static mutex_t s_ring_lock = LWP_MUTEX_NULL;
static sem_t s_log_sem = LWP_SEM_NULL;
static int s_console_off;   /* the game owns the screen: scrolling the console costs a frame copy */
static VIRetraceCallback s_console_cb;   /* libogc's console copy, set aside while the game shows */
static u16 s_console_fb_width;
#define LOG_RING (64 * 1024)
static char s_ring[LOG_RING];
static unsigned s_ring_rd, s_ring_wr, s_ring_lost;   /* free-running byte counts */

/* Write out what the ring holds; the caller holds s_log_lock. */
static void log_drain(void)
{
    static char chunk[4096];
    for (;;) {
        LWP_MutexLock(s_ring_lock);
        unsigned n = s_ring_wr - s_ring_rd;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (unsigned i = 0; i < n; i++) chunk[i] = s_ring[(s_ring_rd + i) % LOG_RING];
        s_ring_rd += n;
        LWP_MutexUnlock(s_ring_lock);
        if (!n) return;
        if (!s_logf) s_logf = fopen("sd:/apps/vibribbon/log.txt", "w");
        if (s_logf) fwrite(chunk, 1, n, s_logf);
    }
}

static void *log_writer(void *arg)
{
    (void)arg;
    for (;;) {
        LWP_SemWait(s_log_sem);
        LWP_MutexLock(s_log_lock);
        log_drain();
        if (s_logf) {
            fflush(s_logf);
            fsync(fileno(s_logf));
        }
        LWP_MutexUnlock(s_log_lock);
    }
    return NULL;
}

void wii_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (s_log_lock == LWP_MUTEX_NULL) {
        lwp_t t;
        LWP_MutexInit(&s_log_lock, false);
        LWP_MutexInit(&s_ring_lock, false);
        LWP_SemInit(&s_log_sem, 0, 0x7FFFFFFF);
        LWP_CreateThread(&t, log_writer, NULL, NULL, 16384, 32);   /* the guest runs at 64 */
    }
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    if (!s_console_off) fputs(line, stdout);
    LWP_MutexLock(s_ring_lock);
    if (LOG_RING - (s_ring_wr - s_ring_rd) >= (unsigned)n) {
        for (int i = 0; i < n; i++) s_ring[(s_ring_wr + i) % LOG_RING] = line[i];
        s_ring_wr += (unsigned)n;
    } else {
        s_ring_lost++;
    }
    LWP_MutexUnlock(s_ring_lock);
    LWP_SemPost(s_log_sem);
}

/* Write everything logged so far before returning. */
static void log_flush(void)
{
    LWP_MutexLock(s_log_lock);
    log_drain();
    if (s_logf) {
        fflush(s_logf);
        fsync(fileno(s_logf));
    }
    LWP_MutexUnlock(s_log_lock);
}

/* Debug aid: write the current PS1 display rectangle as a binary PPM. */
void wii_dump_ppm(const uint16_t *vram, int x, int y, int w, int h, const char *path)
{
    LWP_MutexLock(s_log_lock);
    FILE *f = fopen(path, "wb");
    if (!f) { LWP_MutexUnlock(s_log_lock); return; }
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t p = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
            uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3),
                               (uint8_t)(((p >> 10) & 31) << 3) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    LWP_MutexUnlock(s_log_lock);
}

/* Debug aid: write the XFB on screen as it is scanned out: big-endian width
 * and height in pixels, then the YCbCr 4:2:2 words. */
void wii_dump_xfb(const char *path)
{
    LWP_MutexLock(s_log_lock);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[2] = { s_rmode->fbWidth, s_rmode->xfbHeight };
        fwrite(hdr, 4, 2, f);
        fwrite(s_xfb[s_fb], 2, (size_t)s_rmode->fbWidth * s_rmode->xfbHeight, f);
        fclose(f);
    }
    LWP_MutexUnlock(s_log_lock);
}

/* Debug aid: write a file under the log lock (libfat is not thread-safe). */
void wii_write_file(const char *path, const void *a, unsigned na, const void *b, unsigned nb)
{
    LWP_MutexLock(s_log_lock);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(a, 1, na, f);
        fwrite(b, 1, nb, f);
        fclose(f);
    }
    LWP_MutexUnlock(s_log_lock);
}

void wii_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s_console_off) {   /* give the screen back to the console */
        s_rmode->fbWidth = s_console_fb_width;
        VIDEO_Configure(s_rmode);
        VIDEO_ClearFrameBuffer(s_rmode, s_xfb[s_fb], COLOR_BLACK);
        VIDEO_SetPostRetraceCallback(s_console_cb);
        VIDEO_Flush();
    }
    s_console_off = 0;
    wii_log("\n\nFATAL: %s\n\nPress HOME to exit.\n", msg);
    log_flush();
    INPUT_Init();   /* safe if already up; the SD check fails before it */
    for (;;) {
        VIDEO_WaitVSync();
        INPUT_Update();
        wii_exit_poll(INPUT_PadsHeld());
    }
}

/* Leave for the loader (the Homebrew Channel), or power off, from the main
 * thread: wait for the guest to park, then close what is open, as the WiiCD
 * player does, and return through exit(). */
void wii_exit(int power_off)
{
    for (int i = 0; i < 50 && !s_guest_parked; i++) usleep(10000);
    AUDIO_StopDMA();
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    INPUT_Shutdown();      /* the Wii Remotes (WPAD_Shutdown), GameCube adapter, USB keyboard and mouse */
    wii_usbcd_shutdown();
    wii_log("exit: devices closed (guest %s), %s\n", s_guest_parked ? "parked" : "running",
            power_off ? "powering off" :
            *(volatile uint32_t *)0x80001804 == 0x53545542u ? "back to the loader" : "no loader stub: to the system menu");
    log_flush();
    LWP_MutexLock(s_log_lock);
    if (s_logf) { fclose(s_logf); s_logf = NULL; }
    fatUnmount("sd:");
    LWP_MutexUnlock(s_log_lock);
    if (power_off) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    exit(0);
}

void wii_set_wpad_power_callback(void)
{
    WPAD_SetPowerButtonCallback(on_wpad_power);
}

void wii_exit_poll(uint32_t held)
{
    if (held & WPAD_BUTTON_HOME && !s_exit_req) s_exit_req = 1;
    if (!s_exit_req) return;
    if (LWP_GetSelf() == s_main_thread) wii_exit(s_exit_req == 2);
    s_guest_parked = 1;    /* the main thread takes it from here */
    for (;;) usleep(100000);
}

void wii_present(const uint16_t *vram, int x, int y, int w, int h)
{
    static uint16_t sx_map[720];   /* XFB column -> PS1 column, rebuilt when the mode changes */
    static int map_x = -1, map_w = -1;
    if (w <= 0 || h <= 0) return;
    if (!s_console_off) {
        /* The console copies its whole text buffer onto the displayed XFB at
         * every retrace, at the console's own stride: over the game's picture
         * (in another width once the XFB is resized) that leaves its text as
         * stray dots. Stop it while the game owns the screen. */
        s_console_fb_width = s_rmode->fbWidth;
        s_console_cb = VIDEO_SetPostRetraceCallback(NULL);
        s_console_off = 1;
    }

    /* A display 320 to 640 pixels wide goes into an XFB of the same width and
     * the VI scales it to the screen; others are scaled here. */
    static int full_w;
    if (!full_w) full_w = s_rmode->fbWidth;
    int want_w = (w >= 320 && w <= full_w && w % 16 == 0 && x + w <= 1024) ? w : full_w;
    if (want_w != s_rmode->fbWidth) {
        s_rmode->fbWidth = want_w;
        VIDEO_Configure(s_rmode);
        map_w = -1;
    }
    const int fw = s_rmode->fbWidth, dw = fw / 2, dh = s_rmode->xfbHeight;
    if (x != map_x || w != map_w) {
        for (int dx = 0; dx < fw && dx < 720; dx++)
            sx_map[dx] = (uint16_t)((x + (dx * w) / fw) & 1023);
        map_x = x; map_w = w;
    }

    /* The VI flips to the new XFB at the next retrace; with two buffers and a
     * present every few frames there is no need to block on vsync here.
     * The conversion writes through the cache and flushes once: uncached
     * stores to the XFB are one bus write each. */
    uint32_t *dst = (uint32_t *)MEM_K1_TO_K0(s_xfb[s_fb ^ 1]);
    int prev_sy = -1;
    for (int dy = 0; dy < dh; dy++) {
        int sy = (y + (dy * h) / dh) & 511;
        uint32_t *out = dst + dy * dw;
        if (sy == prev_sy) {                 /* scaled up: repeat the row above */
            memcpy(out, out - dw, (size_t)dw * 4u);
            continue;
        }
        prev_sy = sy;
        const uint16_t *row = vram + (sy << 10);
        if (fw == w) {                       /* one PS1 pixel per XFB pixel */
            const uint16_t *src = row + x;
            uint32_t prev_in = 0, prev_out = 0;
            int have = 0;
            for (int dx = 0; dx < dw; dx++) {
                uint32_t in;                 /* runs of one colour convert once */
                memcpy(&in, src + dx * 2, 4);
                in &= 0x7FFF7FFFu;
                if (have && in == prev_in) { out[dx] = prev_out; continue; }
                uint32_t a = s_ycbcr_lut[in >> 16];
                uint32_t b = s_ycbcr_lut[in & 0x7FFF];
                uint32_t cb = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) >> 1;
                uint32_t cr = ((a & 0xFF) + (b & 0xFF)) >> 1;
                prev_out = out[dx] = ((a >> 16) << 24) | (cb << 16) | ((b >> 16) << 8) | cr;
                prev_in = in;
                have = 1;
            }
            continue;
        }
        for (int dx = 0; dx < dw; dx++) {
            uint32_t a = s_ycbcr_lut[row[sx_map[dx * 2]] & 0x7FFF];
            uint32_t b = s_ycbcr_lut[row[sx_map[dx * 2 + 1]] & 0x7FFF];
            uint32_t cb = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) >> 1;
            uint32_t cr = ((a & 0xFF) + (b & 0xFF)) >> 1;
            out[dx] = ((a >> 16) << 24) | (cb << 16) | ((b >> 16) << 8) | cr;
        }
    }
    DCStoreRange(dst, (u32)dh * dw * 4u);
    s_fb ^= 1;
    VIDEO_SetNextFramebuffer(s_xfb[s_fb]);
    VIDEO_Flush();
}

void wii_stage(int n, const char *what)
{
    wii_log("stage %d: %s\n", n, what);
#ifdef VIB_STOP_AT
    if (n == VIB_STOP_AT) {
        wii_log("stopping at stage %d\n", n);
        log_flush();
        if (s_logf) fclose(s_logf);
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
        for (;;) VIDEO_WaitVSync();
    }
#endif
}
