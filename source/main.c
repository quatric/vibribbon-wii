/*
 * Wii CD Player - plays audio CDs from a USB optical drive.
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbcd.h"
#include "cdplayer.h"
#include "netlog.h"

#define VERSION "0.1.0"

#define COLS        76
#define LIST_ROWS   11
#define LOG_ROWS    3

#define C_RESET   "\x1b[0m"
#define C_TITLE   "\x1b[36;1m"
#define C_DIM     "\x1b[37m"
#define C_BRIGHT  "\x1b[37;1m"
#define C_PLAY    "\x1b[32;1m"
#define C_WARN    "\x1b[33;1m"
#define C_SEL     "\x1b[30;46m"

static void *xfb;
static GXRModeObj *rmode;
static volatile bool want_exit, want_poweroff;

static void reset_cb(u32 irq, void *ctx) { want_exit = true; }
static void power_cb(void) { want_poweroff = true; }
static void wpad_power_cb(s32 chan) { want_poweroff = true; }

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static void init_video(void)
{
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	printf("\x1b[2J");
}

/* print one full-width row at (row) with color, padding with spaces */
static void row(int r, const char *color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void row(int r, const char *color, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > COLS) n = COLS;
	if (n < 0) n = 0;
	memset(buf + n, ' ', COLS - n);
	buf[COLS] = 0;
	printf("\x1b[%d;0H%s%s" C_RESET, r, color, buf);
}

static void fmt_time(char *out, u32 sectors)
{
	u32 s = sectors / CD_FRAMES_PER_SEC;
	sprintf(out, "%02u:%02u", s / 60, s % 60);
}

static const char *track_name(const cd_text_t *txt, const cd_track_t *t, char *tmp)
{
	if (t->number >= 1 && txt->title[t->number - 1][0])
		return txt->title[t->number - 1];
	sprintf(tmp, TRACK_IS_DATA(t) ? "Data track %02d" : "Track %02d", t->number);
	return tmp;
}

static void draw(const player_status_t *s, const cd_toc_t *toc, const cd_text_t *txt, bool have_disc, int cursor)
{
	static const char *rep[] = { "Off", "All", "One" };
	char tmp[64], el[16], len[16], bar[48];
	char logs[LOG_ROWS][80];
	int i, r, first, nlog;

	row(1, C_TITLE, "Wii CD Player " VERSION "%*s", 54, s->drive_name[0] ? s->drive_name : "");
	row(2, C_DIM, "----------------------------------------------------------------------------");

	if (s->drive == DRIVE_NONE) {
		row(3, C_WARN, "No USB optical drive found. Plug the drive into a Wii USB port.");
		row(4, C_DIM, "(Bus-powered slim drives may need a Y-cable using both USB ports.)");
	} else if (have_disc && s->drive == DRIVE_READY) {
		u32 total = toc->leadout;
		fmt_time(len, total);
		if (txt->present && (txt->album[0] || txt->artist[0]))
			row(3, C_BRIGHT, "%s%s%s", txt->album, txt->artist[0] ? " - " : "", txt->artist);
		else
			row(3, C_BRIGHT, "Audio CD");
		row(4, C_DIM, "%d audio track%s, %s total", toc->audio_count, toc->audio_count == 1 ? "" : "s", len);
	} else {
		row(3, C_WARN, "%s", s->status_msg);
		row(4, "", " ");
	}

	/* now playing */
	if (s->track >= 0 && have_disc) {
		const cd_track_t *t = &toc->tracks[s->track];
		const char *perf = txt->performer[t->number - 1];
		int fill = s->track_length ? (int)((u64)s->track_elapsed * 40 / s->track_length) : 0;
		if (fill > 40) fill = 40;
		row(6, s->play == PLAY_PAUSED ? C_WARN : C_PLAY, "%s  %02d  %s",
		    s->play == PLAY_PAUSED ? "|| PAUSED " : ">  PLAYING", t->number, track_name(txt, t, tmp));
		row(7, C_DIM, "                %s", perf[0] ? perf : "");
		memset(bar, '=', fill);
		memset(bar + fill, '-', 40 - fill);
		bar[40] = 0;
		if (fill < 40) bar[fill] = '>';
		fmt_time(el, s->track_elapsed);
		fmt_time(len, s->track_length);
		row(8, C_BRIGHT, "   [%s] %s / %s", bar, el, len);
	} else {
		row(6, C_DIM, "[]  STOPPED");
		row(7, "", " ");
		row(8, "", " ");
	}

	{
		char vb[17];
		int v = s->volume * 16 / 255;
		memset(vb, '#', v);
		memset(vb + v, '.', 16 - v);
		vb[16] = 0;
		row(9, C_DIM, "   Vol [%s]  Repeat: %s  Shuffle: %s  Buffer: %2d/%d%s", vb, rep[s->repeat],
		    s->shuffle ? "On " : "Off", s->buffered, s->buffer_slots, s->underruns ? "  (underruns!)" : "");
	}
	row(10, C_DIM, "----------------------------------------------------------------------------");

	/* track list */
	first = 0;
	if (have_disc && toc->count > LIST_ROWS) {
		first = cursor - LIST_ROWS / 2;
		if (first < 0) first = 0;
		if (first > toc->count - LIST_ROWS) first = toc->count - LIST_ROWS;
	}
	for (r = 0; r < LIST_ROWS; r++) {
		int idx = first + r;
		if (!have_disc || idx >= toc->count) {
			row(11 + r, "", " ");
			continue;
		}
		{
			const cd_track_t *t = &toc->tracks[idx];
			const char *color = idx == cursor ? C_SEL : TRACK_IS_DATA(t) ? C_DIM : (idx == s->track ? C_PLAY : C_BRIGHT);
			fmt_time(len, t->length);
			row(11 + r, color, " %c %02d  %-56.56s %s", idx == s->track ? '*' : ' ', t->number,
			    track_name(txt, t, tmp), TRACK_IS_DATA(t) ? " data" : len);
		}
	}
	row(11 + LIST_ROWS, C_DIM, "----------------------------------------------------------------------------");

	nlog = netlog_tail(logs, LOG_ROWS);
	for (i = 0; i < LOG_ROWS; i++)
		row(12 + LIST_ROWS + i, C_DIM, "%s", i < nlog ? logs[i] : "");

	row(12 + LIST_ROWS + LOG_ROWS, C_TITLE, "A play/pause  B stop/eject  </> track (hold=scan)  +/- vol  1 rpt  2 shuf");
	if (netlog_ip()[0])
		row(13 + LIST_ROWS + LOG_ROWS, C_DIM, "Debug shell: nc %s %d%s   HOME exit", netlog_ip(), NETLOG_PORT,
		    netlog_client_connected() ? " (connected)" : "");
	else
		row(13 + LIST_ROWS + LOG_ROWS, C_DIM, "Network: connecting...   HOME exit");
}

int main(int argc, char **argv)
{
	static cd_toc_t toc;
	static cd_text_t txt;
	player_status_t s;
	int cursor = 0, last_heard = -1;
	u64 hold_start = 0, last_scan = 0, last_draw = 0;
	bool scanned = false;
	int hold_dir = 0;

	init_video();

	/* reserve the MEM2 DMA buffers before anything else grows into MEM2 */
	if (usbcd_init() < 0)
		printf("USB init failed\n");

	netlog_init(true);
	dlog("Wii CD Player %s, IOS%d v%d", VERSION, IOS_GetVersion(), IOS_GetRevision());
	if (IOS_GetVersion() != 58)
		dlog("warning: not running under IOS58, USB 2.0 may be unavailable");

	WPAD_Init();
	PAD_Init();
	SYS_SetResetCallback(reset_cb);
	SYS_SetPowerCallback(power_cb);
	WPAD_SetPowerButtonCallback(wpad_power_cb);

	player_init();

	while (!want_exit && !want_poweroff) {
		u32 wd, wh, wu, gd, gh, gu;
		bool have_disc;
		int dir;

		WPAD_ScanPads();
		PAD_ScanPads();
		wd = WPAD_ButtonsDown(0);
		wh = WPAD_ButtonsHeld(0);
		wu = WPAD_ButtonsUp(0);
		gd = PAD_ButtonsDown(0);
		gh = PAD_ButtonsHeld(0);
		gu = PAD_ButtonsUp(0);

		player_get_status(&s);
		have_disc = player_get_disc(&toc, &txt);
		if (!have_disc)
			cursor = 0;
		else if (cursor >= toc.count)
			cursor = toc.count - 1;
		if (s.track != last_heard) {
			if (s.track >= 0)
				cursor = s.track;
			last_heard = s.track;
		}

		if ((wd & WPAD_BUTTON_HOME) || (gd & PAD_BUTTON_START))
			break;

		if ((wd & WPAD_BUTTON_A) || (gd & PAD_BUTTON_A)) {
			if (s.play == PLAY_STOPPED || (s.track != cursor && have_disc))
				player_cmd(CMD_PLAY_TRACK, cursor);
			else
				player_cmd(CMD_TOGGLE_PAUSE, cursor);
		}
		if ((wd & WPAD_BUTTON_B) || (gd & PAD_BUTTON_B)) {
			if (s.play == PLAY_STOPPED)
				player_cmd(CMD_EJECT, 0);
			else
				player_cmd(CMD_STOP, 0);
		}
		if (gd & PAD_TRIGGER_Z)
			player_cmd(CMD_EJECT, 0);

		if (have_disc && ((wd & WPAD_BUTTON_UP) || (gd & PAD_BUTTON_UP)) && cursor > 0)
			cursor--;
		if (have_disc && ((wd & WPAD_BUTTON_DOWN) || (gd & PAD_BUTTON_DOWN)) && cursor < toc.count - 1)
			cursor++;

		if ((wd & WPAD_BUTTON_PLUS) || (gd & PAD_TRIGGER_R))
			player_cmd(CMD_VOLUME, s.volume + 16 > 255 ? 255 : s.volume + 16);
		if ((wd & WPAD_BUTTON_MINUS) || (gd & PAD_TRIGGER_L))
			player_cmd(CMD_VOLUME, s.volume - 16 < 0 ? 0 : s.volume - 16);
		if ((wd & WPAD_BUTTON_1) || (gd & PAD_BUTTON_X))
			player_cmd(CMD_REPEAT, 0);
		if ((wd & WPAD_BUTTON_2) || (gd & PAD_BUTTON_Y))
			player_cmd(CMD_SHUFFLE, 0);

		/* Left/Right: tap = prev/next track, hold = scan like a real CD deck */
		dir = 0;
		if ((wd & WPAD_BUTTON_LEFT) || (gd & PAD_BUTTON_LEFT)) dir = -1;
		if ((wd & WPAD_BUTTON_RIGHT) || (gd & PAD_BUTTON_RIGHT)) dir = 1;
		if (dir) {
			hold_dir = dir;
			hold_start = now_ms();
			scanned = false;
		}
		if (hold_dir) {
			bool held = hold_dir < 0 ? ((wh & WPAD_BUTTON_LEFT) || (gh & PAD_BUTTON_LEFT))
			                         : ((wh & WPAD_BUTTON_RIGHT) || (gh & PAD_BUTTON_RIGHT));
			bool released = hold_dir < 0 ? ((wu & WPAD_BUTTON_LEFT) || (gu & PAD_BUTTON_LEFT))
			                             : ((wu & WPAD_BUTTON_RIGHT) || (gu & PAD_BUTTON_RIGHT));
			u64 t = now_ms();
			if (held && s.play != PLAY_STOPPED && t - hold_start > 500 && t - last_scan > 300) {
				player_cmd(CMD_SEEK, hold_dir * 5);
				last_scan = t;
				scanned = true;
			}
			if (released || !held) {
				if (!scanned) {
					if (s.play != PLAY_STOPPED)
						player_cmd(hold_dir < 0 ? CMD_PREV : CMD_NEXT, 0);
					else if (have_disc)
						cursor = hold_dir < 0 ? (cursor > 0 ? cursor - 1 : 0)
						                      : (cursor < toc.count - 1 ? cursor + 1 : cursor);
				}
				hold_dir = 0;
			}
		}

		if (now_ms() - last_draw >= 66) {
			draw(&s, &toc, &txt, have_disc, cursor);
			last_draw = now_ms();
		}
		VIDEO_WaitVSync();
	}

	printf("\x1b[2J\x1b[2;0HShutting down...\n");
	dlog("exiting");
	player_shutdown();
	usbcd_close();
	netlog_shutdown();
	WPAD_Shutdown();

	if (want_poweroff)
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	return 0;
}
