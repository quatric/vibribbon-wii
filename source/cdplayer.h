#ifndef CDPLAYER_H
#define CDPLAYER_H

#include <gctypes.h>
#include <stdbool.h>
#include "usbcd.h"

typedef enum {
	DRIVE_NONE,          /* no USB optical drive attached */
	DRIVE_NO_DISC,
	DRIVE_TRAY_OPEN,
	DRIVE_SPINUP,        /* disc present, becoming ready */
	DRIVE_NO_AUDIO,      /* disc loaded but has no audio tracks */
	DRIVE_READY,         /* TOC loaded */
} drive_state_t;

typedef enum { PLAY_STOPPED, PLAY_PLAYING, PLAY_PAUSED } play_state_t;
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_mode_t;

typedef enum {
	CMD_PLAY_TRACK,      /* arg = track index into toc.tracks */
	CMD_TOGGLE_PAUSE,
	CMD_STOP,
	CMD_NEXT,
	CMD_PREV,
	CMD_SEEK,            /* arg = relative seconds */
	CMD_EJECT,           /* eject or load, depending on tray state */
	CMD_VOLUME,          /* arg = 0..255 */
	CMD_REPEAT,          /* cycle */
	CMD_SHUFFLE,         /* toggle */
	CMD_SPEED,           /* arg = kB/s, 0 = drive max */
	CMD_SWAP,            /* toggle sample byte-swap */
	CMD_RELOAD,          /* re-read TOC */
	CMD_TONE,            /* toggle test tone (440 Hz left, 1 kHz right) */
} player_cmd_t;

typedef struct {
	drive_state_t drive;
	play_state_t  play;
	repeat_mode_t repeat;
	bool shuffle;
	bool swap;
	bool tone;
	int  volume;
	int  track;          /* index in toc of the track being heard, -1 if none */
	u32  track_elapsed;  /* sectors into the current track */
	u32  track_length;
	int  buffered;       /* slots filled ahead */
	int  buffer_slots;
	u32  underruns;
	u32  read_errors;
	u32  read_ms_avg;
	u16  speed;
	char drive_name[48];
	char status_msg[64];
} player_status_t;

void player_init(void);
void player_shutdown(void);
void player_cmd(player_cmd_t cmd, int arg);
void player_get_status(player_status_t *st);
/* copies the TOC/CD-TEXT; returns false if no disc loaded */
bool player_get_disc(cd_toc_t *toc, cd_text_t *txt);

#endif
