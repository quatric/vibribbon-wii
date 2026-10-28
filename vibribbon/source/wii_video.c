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
static uint32_t s_ycbcr_lut[32768];   /* RGB555 -> one YCbCr pixel (Y Cb Y Cr uses pairs) */

static uint32_t rgb555_to_ycbcr_pair(uint16_t a, uint16_t b)
{
    int r1 = (a & 31) << 3, g1 = ((a >> 5) & 31) << 3, b1 = ((a >> 10) & 31) << 3;
    int r2 = (b & 31) << 3, g2 = ((b >> 5) & 31) << 3, b2 = ((b >> 10) & 31) << 3;
    int y1 = (299 * r1 + 587 * g1 + 114 * b1) / 1000;
    int y2 = (299 * r2 + 587 * g2 + 114 * b2) / 1000;
    int r = (r1 + r2) >> 1, g = (g1 + g2) >> 1, bl = (b1 + b2) >> 1;
    int cb = 128 + (-169 * r - 331 * g + 500 * bl) / 1000;
    int cr = 128 + (500 * r - 419 * g - 81 * bl) / 1000;
    if (cb < 0) cb = 0; if (cb > 255) cb = 255;
    if (cr < 0) cr = 0; if (cr > 255) cr = 255;
    return ((uint32_t)y1 << 24) | ((uint32_t)cb << 16) | ((uint32_t)y2 << 8) | (uint32_t)cr;
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
    CON_InitEx(s_rmode, 20, 20, s_rmode->fbWidth - 40, s_rmode->xfbHeight - 40);
}

static FILE *s_logf;

void wii_log(const char *fmt, ...)
{
    va_list ap;
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
}

/* Debug aid: write the current PS1 display rectangle as a binary PPM. */
void wii_dump_ppm(const uint16_t *vram, int x, int y, int w, int h, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
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
}

void wii_fatal(const char *fmt, ...)
{
    va_list ap;
    printf("\n\nFATAL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n\nPress HOME to exit.\n");
    for (;;) VIDEO_WaitVSync();
}

void wii_present(const uint16_t *vram, int x, int y, int w, int h)
{
    static int lut_ready;
    if (!lut_ready) lut_ready = 1;   /* pair conversion is done inline below */
    if (w <= 0 || h <= 0) return;

    uint32_t *dst = (uint32_t *)s_xfb[s_fb ^ 1];
    const int dw = s_rmode->fbWidth / 2;          /* pixel pairs per line */
    const int dh = s_rmode->xfbHeight;
    for (int dy = 0; dy < dh; dy++) {
        const int sy = y + (dy * h) / dh;
        const uint16_t *row = vram + ((sy & 511) << 10);
        uint32_t *out = dst + dy * (s_rmode->fbWidth / 2);
        for (int dx = 0; dx < dw; dx++) {
            const int sx0 = x + ((dx * 2) * w) / s_rmode->fbWidth;
            const int sx1 = x + ((dx * 2 + 1) * w) / s_rmode->fbWidth;
            out[dx] = rgb555_to_ycbcr_pair(row[sx0 & 1023], row[sx1 & 1023]);
        }
    }
    s_fb ^= 1;
    VIDEO_SetNextFramebuffer(s_xfb[s_fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
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
