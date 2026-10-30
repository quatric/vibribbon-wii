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
/* HOME, the console's Reset button: back to the loader; Power: power off. */
void wii_exit(int power_off) __attribute__((noreturn));
/* Call once per frame: on the main thread it exits; any other thread parks
 * there for the main thread to exit. */
void wii_exit_poll(uint32_t held_buttons);
void wii_set_wpad_power_callback(void);
void wii_stage(int n, const char *what);   /* log a bring-up stage; powers off at VIB_STOP_AT */
void wii_dump_ppm(const uint16_t *vram, int x, int y, int w, int h, const char *path);
void wii_dump_xfb(const char *path);

/* Reference text for the native-code guard of each main program
 * (0 = MAIN_G game, 1 = MAIN_T title, 2 = MAIN_K How to Play); see wii_dispatch.c. */
#define WII_MAIN_PROGS 3
void wii_dispatch_set_text(int prog, uint32_t lo, const uint8_t *img, uint32_t len);

/* Present the PS1 VRAM display area on the TV (called every vblank). */
void wii_present(const uint16_t *vram_1024x512, int x, int y, int w, int h);

/* A USB CD drive as the PlayStation's (wii_usbcd.c): iso_open(WII_USBCD_PATH)
 * mounts the music CD in it. The frame hook takes the drive's events and
 * swaps discs. */
#define WII_USBCD_PATH     "usbcd:"
#define WII_USBCD_INSERTED 1
#define WII_USBCD_REMOVED  2
void wii_usbcd_init(void);
void wii_usbcd_shutdown(void);
int  wii_usbcd_take_event(void);
int  wii_usbcd_read(uint32_t lba, uint8_t *out_2352);

/* SPU output (wii_audio.c): frames queued at 48 kHz, and cumulative stats. */
void wii_audio_init(void);
uint32_t wii_audio_queued(void);
void wii_audio_stats(unsigned *underruns, unsigned *dropped, unsigned *rendered);  /* SPU frames since last call */

#ifdef __cplusplus
}
#endif

#endif
