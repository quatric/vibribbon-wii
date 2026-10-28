#ifndef WII_HOST_H
#define WII_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void wii_video_init(void);
void wii_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void wii_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wii_run_game(void);
void wii_dump_ppm(const uint16_t *vram, int x, int y, int w, int h, const char *path);

/* Present the PS1 VRAM display area on the TV (called every vblank). */
void wii_present(const uint16_t *vram_1024x512, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
