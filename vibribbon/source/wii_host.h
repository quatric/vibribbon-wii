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
void wii_stage(int n, const char *what);   /* log a bring-up stage; powers off at VIB_STOP_AT */
void wii_dump_ppm(const uint16_t *vram, int x, int y, int w, int h, const char *path);

/* Reference text for the native-code guard of each main program
 * (0 = MAIN_G game, 1 = MAIN_T title); see wii_dispatch.c. */
#define WII_MAIN_PROGS 2
void wii_dispatch_set_text(int prog, uint32_t lo, const uint8_t *img, uint32_t len);

/* Present the PS1 VRAM display area on the TV (called every vblank). */
void wii_present(const uint16_t *vram_1024x512, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
