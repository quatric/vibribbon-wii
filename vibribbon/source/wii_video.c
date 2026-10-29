/* Video output: the PS1 display rectangle is converted from 15-bit RGB VRAM
 * into the Wii's YCbCr external framebuffer and scaled to the TV. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>

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

void wii_video_init(void)
{
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

static FILE *s_logf;
static mutex_t s_log_lock = LWP_MUTEX_NULL;   /* libfat is not thread-safe */

void wii_log(const char *fmt, ...)
{
    va_list ap;
    if (s_log_lock == LWP_MUTEX_NULL) LWP_MutexInit(&s_log_lock, false);
    LWP_MutexLock(s_log_lock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (!s_logf) s_logf = fopen("sd:/apps/vibribbon/log.txt", "w");
    if (s_logf) {
        va_start(ap, fmt);
        vfprintf(s_logf, fmt, ap);
        va_end(ap);
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
    wii_log("\n\nFATAL: %s\n\nPress HOME to exit.\n", msg);
    for (;;) VIDEO_WaitVSync();
}

void wii_present(const uint16_t *vram, int x, int y, int w, int h)
{
    static uint16_t sx_map[720];   /* XFB column -> PS1 column, rebuilt when the mode changes */
    static int map_x = -1, map_w = -1;
    if (w <= 0 || h <= 0) return;

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
    for (int dy = 0; dy < dh; dy++) {
        const uint16_t *row = vram + (((y + (dy * h) / dh) & 511) << 10);
        uint32_t *out = dst + dy * dw;
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
        if (s_logf) fclose(s_logf);
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
        for (;;) VIDEO_WaitVSync();
    }
#endif
}
