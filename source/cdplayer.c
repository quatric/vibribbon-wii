/*
 * cdplayer - drive management + CD-DA streaming.
 *
 * Two threads:
 *   drive thread  owns the USB drive: polls for drive/disc, loads the TOC,
 *                 runs commands and reads CD-DA ahead into a ring of slots.
 *   feeder thread owns ASND: queues filled slots into a double-buffered
 *                 voice at 44.1 kHz (ASND resamples to the DSP's 48 kHz) and
 *                 reclaims slots once the DSP is done with them.
 */
#include <gccore.h>
#include <asndlib.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cdplayer.h"
#include "netlog.h"
#include "metadata.h"

#define VOICE          0
#define SLOT_SECTORS   8                                 /* ~107 ms per slot */
#define SLOT_BYTES     (SLOT_SECTORS * CD_FRAME_BYTES)   /* 18816, multiple of 32 */
#define NSLOTS         32                                /* ~3.4 s read-ahead */
#define PREBUFFER      4
#define DEFAULT_SPEED  706                               /* 4x, quieter than max */

enum { SLOT_FREE, SLOT_FILLED, SLOT_QUEUED };

typedef struct {
	u8 *pcm;
	u32 size;
	u32 lba;
	s16 track;
	volatile u8 state;
} slot_t;

static slot_t slots[NSLOTS];
static u32 w_idx, q_idx, r_idx;
static bool voice_started, in_underrun;
static volatile bool reader_eof, finished;
static mutex_t ring_lock;

/* shared status, guarded by state_lock */
static mutex_t state_lock;
static player_status_t st;
static cd_toc_t toc;
static cd_text_t cdtext;
static volatile int heard_track = -1;
static volatile u32 heard_lba;

/* command queue */
#define CMDQ_LEN 16
static struct { player_cmd_t cmd; int arg; } cmdq[CMDQ_LEN];
static u32 cmdq_head, cmdq_tail;
static mutex_t cmd_lock;

/* drive-thread private */
static int order[CD_MAX_TRACKS], order_len, order_pos;
static int rd_track = -1;
static u32 rd_lba, rd_end;
static bool reading;
static u32 read_ms_total, read_count;
static int transport_errors;

static lwp_t drive_thread = LWP_THREAD_NULL, feeder_thread = LWP_THREAD_NULL;
static volatile bool quit;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static void set_msg(const char *m)
{
	LWP_MutexLock(state_lock);
	strncpy(st.status_msg, m, sizeof(st.status_msg) - 1);
	st.status_msg[sizeof(st.status_msg) - 1] = 0;
	LWP_MutexUnlock(state_lock);
}

static void set_drive(drive_state_t d)
{
	LWP_MutexLock(state_lock);
	if (st.drive != d) {
		static const char *names[] = { "none", "no disc", "tray open", "spin-up", "no audio", "ready" };
		dlog("drive: %s -> %s", names[st.drive], names[d]);
	}
	st.drive = d;
	LWP_MutexUnlock(state_lock);
}

static void set_play(play_state_t p)
{
	LWP_MutexLock(state_lock);
	st.play = p;
	LWP_MutexUnlock(state_lock);
}

static void asnd_cb(s32 voice) { /* presence keeps the voice alive between buffers */ }

/* Drop everything buffered and silence the voice. Drive thread only. */
static void ring_flush(void)
{
	int i;
	LWP_MutexLock(ring_lock);
	ASND_StopVoice(VOICE);
	for (i = 0; i < NSLOTS; i++)
		slots[i].state = SLOT_FREE;
	w_idx = q_idx = r_idx = 0;
	voice_started = false;
	in_underrun = false;
	reader_eof = false;
	finished = false;
	LWP_MutexUnlock(ring_lock);
}

static void *feeder_main(void *arg)
{
	while (!quit) {
		play_state_t ps;
		int vol;

		LWP_MutexLock(state_lock);
		ps = st.play;
		vol = st.volume;
		LWP_MutexUnlock(state_lock);

		LWP_MutexLock(ring_lock);
		if (ps != PLAY_STOPPED) {
			/* reclaim slots the DSP has finished with, oldest first */
			while (slots[r_idx].state == SLOT_QUEUED && ASND_TestPointer(VOICE, slots[r_idx].pcm) == 0) {
				slots[r_idx].state = SLOT_FREE;
				r_idx = (r_idx + 1) % NSLOTS;
			}

			if (ps == PLAY_PLAYING && !voice_started) {
				int n = 0;
				while (n < NSLOTS && slots[(q_idx + n) % NSLOTS].state == SLOT_FILLED)
					n++;
				if (n >= PREBUFFER || (reader_eof && n > 0)) {
					slot_t *s = &slots[q_idx];
					ASND_SetVoice(VOICE, VOICE_STEREO_16BIT_LE, 44100, 0, s->pcm, s->size, vol, vol, asnd_cb);
					ASND_PauseVoice(VOICE, 0);
					s->state = SLOT_QUEUED;
					r_idx = q_idx;
					q_idx = (q_idx + 1) % NSLOTS;
					voice_started = true;
					in_underrun = false;
				}
			} else if (voice_started) {
				if (slots[q_idx].state == SLOT_FILLED && ASND_TestVoiceBufferReady(VOICE) == 1) {
					slot_t *s = &slots[q_idx];
					if (ASND_AddVoice(VOICE, s->pcm, s->size) == SND_OK) {
						s->state = SLOT_QUEUED;
						q_idx = (q_idx + 1) % NSLOTS;
						in_underrun = false;
					}
				}
				if (slots[r_idx].state != SLOT_QUEUED && slots[q_idx].state != SLOT_FILLED) {
					/* nothing playing and nothing ready */
					if (reader_eof) {
						finished = true;
					} else if (ps == PLAY_PLAYING && !in_underrun) {
						in_underrun = true;
						st.underruns++;
						dlog("audio: buffer underrun (%u)", st.underruns);
					}
				}
			}

			if (slots[r_idx].state == SLOT_QUEUED) {
				heard_track = slots[r_idx].track;
				heard_lba = slots[r_idx].lba;
			}
		}
		LWP_MutexUnlock(ring_lock);
		usleep(3000);
	}
	return NULL;
}

/* ---- drive thread ---- */

static void build_order(int keep_first)
{
	int i, j, t;
	order_len = 0;
	for (i = 0; i < toc.count; i++)
		if (!TRACK_IS_DATA(&toc.tracks[i]) && toc.tracks[i].length > 0)
			order[order_len++] = i;

	if (st.shuffle && order_len > 1) {
		for (i = order_len - 1; i > 0; i--) {
			j = rand() % (i + 1);
			t = order[i]; order[i] = order[j]; order[j] = t;
		}
	}
	order_pos = 0;
	if (keep_first >= 0) {
		for (i = 0; i < order_len; i++) {
			if (order[i] == keep_first) {
				if (st.shuffle) {
					t = order[0]; order[0] = order[i]; order[i] = t;
				} else {
					order_pos = i;
				}
				break;
			}
		}
	}
}

static void start_reading(int track_idx, u32 lba)
{
	cd_track_t *t = &toc.tracks[track_idx];
	ring_flush();
	rd_track = track_idx;
	rd_lba = lba;
	rd_end = t->lba + t->length;
	reading = true;
}

static void advance_track(void)
{
	if (st.repeat == REPEAT_ONE) {
		rd_lba = toc.tracks[rd_track].lba;
		return;
	}
	order_pos++;
	if (order_pos >= order_len) {
		if (st.repeat == REPEAT_ALL && order_len > 0) {
			build_order(-1);
		} else {
			reading = false;
			reader_eof = true;
			return;
		}
	}
	rd_track = order[order_pos];
	rd_lba = toc.tracks[rd_track].lba;
	rd_end = rd_lba + toc.tracks[rd_track].length;
}

static void unload_disc(void)
{
	ring_flush();
	reading = false;
	set_play(PLAY_STOPPED);
	heard_track = -1;
	LWP_MutexLock(state_lock);
	memset(&toc, 0, sizeof(toc));
	memset(&cdtext, 0, sizeof(cdtext));
	LWP_MutexUnlock(state_lock);
	meta_request(NULL);
}

static void play_track(int idx, u32 lba)
{
	int i;
	if (st.drive != DRIVE_READY || idx < 0 || idx >= toc.count || TRACK_IS_DATA(&toc.tracks[idx]))
		return;
	for (i = 0; i < order_len && order[i] != idx; i++)
		;
	if (i == order_len || st.shuffle)
		build_order(idx);
	else
		order_pos = i;
	start_reading(idx, lba ? lba : toc.tracks[idx].lba);
	heard_track = idx;
	heard_lba = rd_lba;
	set_play(PLAY_PLAYING);
	dlog("play: track %d from lba %u", toc.tracks[idx].number, rd_lba);
}

static int current_track(void)
{
	int t = heard_track;
	return t >= 0 ? t : rd_track;
}

static void load_disc(void)
{
	cd_toc_t ntoc;
	cd_text_t ntxt;
	int r, i;

	r = usbcd_read_toc(&ntoc);
	if (r != USBCD_OK) {
		cd_sense_t s = usbcd_last_sense();
		dlog("toc: read failed %d (%x/%02x/%02x %s)", r, s.key, s.asc, s.ascq, usbcd_sense_str(s));
		set_drive(DRIVE_SPINUP);
		return;
	}
	if (usbcd_read_cdtext(&ntxt) != USBCD_OK)
		memset(&ntxt, 0, sizeof(ntxt));

	LWP_MutexLock(state_lock);
	toc = ntoc;
	cdtext = ntxt;
	LWP_MutexUnlock(state_lock);

	dlog("toc: tracks %d-%d, %d audio, leadout %u (%u:%02u)", ntoc.first, ntoc.last, ntoc.audio_count,
	     ntoc.leadout, ntoc.leadout / 75 / 60, (ntoc.leadout / 75) % 60);
	for (i = 0; i < ntoc.count; i++) {
		cd_track_t *t = &ntoc.tracks[i];
		dlog("  %02d %s lba %6u len %6u %s", t->number, TRACK_IS_DATA(t) ? "data " : "audio",
		     t->lba, t->length, ntxt.title[t->number - 1]);
	}
	if (ntxt.present)
		dlog("cdtext: \"%s\" by \"%s\"", ntxt.album, ntxt.artist);

	r = usbcd_set_speed(st.speed ? st.speed : 0xFFFF);
	if (r != USBCD_OK)
		dlog("speed: SET CD SPEED not accepted (%d)", r);

	build_order(-1);
	if (ntoc.audio_count > 0)
		meta_request(&ntoc);
	set_drive(ntoc.audio_count > 0 ? DRIVE_READY : DRIVE_NO_AUDIO);
	set_msg(ntoc.audio_count > 0 ? "Disc ready" : "No audio tracks on this disc");
}

/* Interpret a TEST UNIT READY result while no disc is loaded. */
static void handle_tur(int r)
{
	cd_sense_t s;

	if (r < 0) {
		if (++transport_errors >= 3) {
			dlog("drive: lost contact (%d), closing", r);
			usbcd_close();
			transport_errors = 0;
		}
		return;
	}
	transport_errors = 0;

	if (r == USBCD_OK) {
		if (st.drive != DRIVE_READY && st.drive != DRIVE_NO_AUDIO) {
			set_drive(DRIVE_SPINUP);
			set_msg("Reading TOC...");
			load_disc();
		}
		return;
	}

	s = usbcd_last_sense();
	if (s.key == 0x06) {
		/* media changed / reset: drop what we know and re-check next poll */
		if (st.drive == DRIVE_READY || st.drive == DRIVE_NO_AUDIO)
			unload_disc();
		set_drive(DRIVE_SPINUP);
		return;
	}
	if (st.drive == DRIVE_READY || st.drive == DRIVE_NO_AUDIO)
		unload_disc();
	if (s.key == 0x02 && s.asc == 0x3A) {
		set_drive(s.ascq == 0x02 ? DRIVE_TRAY_OPEN : DRIVE_NO_DISC);
		set_msg(s.ascq == 0x02 ? "Tray open" : "No disc");
	} else if (s.key == 0x02 && s.asc == 0x04) {
		set_drive(DRIVE_SPINUP);
		set_msg("Disc spinning up...");
	} else {
		set_msg(usbcd_sense_str(s));
	}
}

static void do_cmd(player_cmd_t cmd, int arg)
{
	int cur;

	switch (cmd) {
	case CMD_PLAY_TRACK:
		play_track(arg, 0);
		break;
	case CMD_TOGGLE_PAUSE:
		if (st.drive != DRIVE_READY)
			break;
		if (st.play == PLAY_STOPPED) {
			play_track(arg >= 0 ? arg : (order_len ? order[0] : -1), 0);
		} else if (st.play == PLAY_PLAYING) {
			ASND_PauseVoice(VOICE, 1);
			set_play(PLAY_PAUSED);
		} else {
			ASND_PauseVoice(VOICE, 0);
			set_play(PLAY_PLAYING);
		}
		break;
	case CMD_STOP:
		ring_flush();
		reading = false;
		heard_track = -1;
		set_play(PLAY_STOPPED);
		break;
	case CMD_NEXT:
	case CMD_PREV:
		if (st.play == PLAY_STOPPED || order_len == 0)
			break;
		cur = current_track();
		{
			int i, pos = 0;
			bool paused = st.play == PLAY_PAUSED;
			for (i = 0; i < order_len; i++)
				if (order[i] == cur) pos = i;
			if (cmd == CMD_PREV && heard_lba > toc.tracks[cur].lba + 3 * CD_FRAMES_PER_SEC) {
				/* restart current track */
			} else if (cmd == CMD_PREV) {
				pos = pos > 0 ? pos - 1 : (st.repeat == REPEAT_ALL ? order_len - 1 : 0);
			} else {
				if (pos + 1 < order_len) pos++;
				else if (st.repeat == REPEAT_ALL) pos = 0;
				else break;
			}
			order_pos = pos;
			start_reading(order[pos], toc.tracks[order[pos]].lba);
			heard_track = order[pos];
			heard_lba = rd_lba;
			set_play(paused ? PLAY_PAUSED : PLAY_PLAYING);
		}
		break;
	case CMD_SEEK:
		if (st.play == PLAY_STOPPED)
			break;
		cur = current_track();
		if (cur >= 0) {
			cd_track_t *t = &toc.tracks[cur];
			s32 nl = (s32)heard_lba + arg * CD_FRAMES_PER_SEC;
			if (nl < (s32)t->lba) nl = t->lba;
			if (nl >= (s32)(t->lba + t->length)) nl = t->lba + t->length - 1;
			start_reading(cur, nl);
			heard_track = cur;
			heard_lba = nl;
		}
		break;
	case CMD_EJECT:
		if (st.drive == DRIVE_NONE)
			break;
		if (st.drive == DRIVE_TRAY_OPEN) {
			set_msg("Closing tray...");
			usbcd_start_stop(true, true);
			set_drive(DRIVE_SPINUP);
		} else {
			unload_disc();
			set_msg("Ejecting...");
			usbcd_allow_removal(true);
			if (usbcd_start_stop(true, false) == USBCD_OK)
				set_drive(DRIVE_TRAY_OPEN);
		}
		break;
	case CMD_VOLUME:
		LWP_MutexLock(state_lock);
		st.volume = arg < 0 ? 0 : arg > 255 ? 255 : arg;
		LWP_MutexUnlock(state_lock);
		ASND_ChangeVolumeVoice(VOICE, st.volume, st.volume);
		break;
	case CMD_REPEAT:
		st.repeat = (st.repeat + 1) % 3;
		break;
	case CMD_SHUFFLE:
		st.shuffle = !st.shuffle;
		build_order(current_track());
		break;
	case CMD_SPEED:
		st.speed = arg;
		dlog("speed: %u kB/s -> %d", st.speed, usbcd_set_speed(arg ? arg : 0xFFFF));
		break;
	case CMD_SWAP:
		st.swap = !st.swap;
		dlog("audio: byte swap %s", st.swap ? "on" : "off");
		break;
	case CMD_RELOAD:
		if (st.drive != DRIVE_NONE) {
			unload_disc();
			set_drive(DRIVE_SPINUP);
		}
		break;
	}
}

static void read_chunk(void)
{
	slot_t *s;
	u32 n, t0, ms;
	int r = -1, attempt;

	LWP_MutexLock(ring_lock);
	s = &slots[w_idx];
	if (s->state != SLOT_FREE) {
		LWP_MutexUnlock(ring_lock);
		usleep(5000);
		return;
	}
	LWP_MutexUnlock(ring_lock);

	n = rd_end - rd_lba;
	if (n > SLOT_SECTORS)
		n = SLOT_SECTORS;

	t0 = now_ms();
	for (attempt = 0; attempt < 3; attempt++) {
		r = usbcd_read_audio(rd_lba, n, s->pcm);
		if (r == USBCD_OK || r == USBCD_ENODEV)
			break;
		if (r == USBCD_CHECK) {
			cd_sense_t se = usbcd_last_sense();
			if (se.key == 0x02 && se.asc == 0x3A)
				break;   /* disc gone */
			if (se.key == 0x06)
				continue;
		}
	}
	ms = now_ms() - t0;
	read_ms_total += ms;
	read_count++;

	if (r != USBCD_OK) {
		cd_sense_t se = usbcd_last_sense();
		st.read_errors++;
		dlog("read: lba %u x%u failed %d (%x/%02x/%02x %s)", rd_lba, n, r, se.key, se.asc, se.ascq,
		     usbcd_sense_str(se));
		if (r == USBCD_ENODEV || (r == USBCD_CHECK && se.key == 0x02 && se.asc == 0x3A)) {
			unload_disc();
			handle_tur(r);
			return;
		}
		if (r < 0)
			transport_errors++;
		memset(s->pcm, 0, n * CD_FRAME_BYTES);   /* conceal as silence and move on */
	} else {
		transport_errors = 0;
	}

	if (st.swap) {
		u16 *p = (u16 *)s->pcm;
		u32 i, cnt = n * CD_FRAME_BYTES / 2;
		for (i = 0; i < cnt; i++)
			p[i] = (p[i] << 8) | (p[i] >> 8);
	}

	s->size = (n * CD_FRAME_BYTES + 31) & ~31;
	if (s->size > n * CD_FRAME_BYTES)
		memset(s->pcm + n * CD_FRAME_BYTES, 0, s->size - n * CD_FRAME_BYTES);
	DCFlushRange(s->pcm, s->size);
	s->lba = rd_lba;
	s->track = rd_track;

	LWP_MutexLock(ring_lock);
	s->state = SLOT_FILLED;
	w_idx = (w_idx + 1) % NSLOTS;
	LWP_MutexUnlock(ring_lock);

	rd_lba += n;
	if (rd_lba >= rd_end)
		advance_track();
}

static void *drive_main(void *arg)
{
	u64 next_poll = 0;

	while (!quit) {
		u64 t = now_ms();

		/* commands */
		for (;;) {
			player_cmd_t c;
			int a;
			LWP_MutexLock(cmd_lock);
			if (cmdq_head == cmdq_tail) {
				LWP_MutexUnlock(cmd_lock);
				break;
			}
			c = cmdq[cmdq_tail].cmd;
			a = cmdq[cmdq_tail].arg;
			cmdq_tail = (cmdq_tail + 1) % CMDQ_LEN;
			LWP_MutexUnlock(cmd_lock);
			do_cmd(c, a);
		}

		if (!usbcd_is_open()) {
			if (st.drive != DRIVE_NONE) {
				unload_disc();
				set_drive(DRIVE_NONE);
				set_msg("Drive disconnected");
				LWP_MutexLock(state_lock);
				st.drive_name[0] = 0;
				LWP_MutexUnlock(state_lock);
			}
			if (t >= next_poll) {
				next_poll = t + 1000;
				if (usbcd_probe()) {
					LWP_MutexLock(state_lock);
					strncpy(st.drive_name, usbcd_name(), sizeof(st.drive_name) - 1);
					LWP_MutexUnlock(state_lock);
					set_drive(DRIVE_NO_DISC);
					set_msg("Drive found");
					transport_errors = 0;
					next_poll = 0;
				}
			}
			usleep(20000);
			continue;
		}

		if (st.drive != DRIVE_READY && st.drive != DRIVE_NO_AUDIO) {
			if (t >= next_poll) {
				next_poll = t + 500;
				handle_tur(usbcd_test_unit_ready());
			}
			usleep(20000);
			continue;
		}

		if (st.play != PLAY_STOPPED) {
			if (finished) {
				dlog("play: end of disc");
				ring_flush();
				reading = false;
				heard_track = -1;
				set_play(PLAY_STOPPED);
				continue;
			}
			if (reading) {
				read_chunk();
				if (transport_errors >= 3) {
					dlog("drive: repeated transport errors, closing");
					usbcd_close();
				}
			} else {
				usleep(5000);
			}
			continue;
		}

		/* idle with a disc: watch for disc change */
		if (t >= next_poll) {
			int r;
			next_poll = t + 2000;
			r = usbcd_test_unit_ready();
			if (r != USBCD_OK)
				handle_tur(r);
		}
		usleep(20000);
	}
	return NULL;
}

void player_init(void)
{
	int i;

	LWP_MutexInit(&state_lock, false);
	LWP_MutexInit(&ring_lock, false);
	LWP_MutexInit(&cmd_lock, false);

	memset(&st, 0, sizeof(st));
	st.volume = 255;
	st.speed = DEFAULT_SPEED;
	st.buffer_slots = NSLOTS;
	strcpy(st.status_msg, "Looking for USB drive...");
	srand(gettime());

	for (i = 0; i < NSLOTS; i++) {
		slots[i].pcm = memalign(32, SLOT_BYTES);
		memset(slots[i].pcm, 0, SLOT_BYTES);
		slots[i].state = SLOT_FREE;
	}

	ASND_Init();
	ASND_Pause(0);

	LWP_CreateThread(&feeder_thread, feeder_main, NULL, NULL, 16 * 1024, 100);
	LWP_CreateThread(&drive_thread, drive_main, NULL, NULL, 64 * 1024, 70);
}

void player_shutdown(void)
{
	quit = true;
	if (drive_thread != LWP_THREAD_NULL)
		LWP_JoinThread(drive_thread, NULL);
	if (feeder_thread != LWP_THREAD_NULL)
		LWP_JoinThread(feeder_thread, NULL);
	ASND_StopVoice(VOICE);
	ASND_End();
}

void player_cmd(player_cmd_t cmd, int arg)
{
	LWP_MutexLock(cmd_lock);
	if ((cmdq_head + 1) % CMDQ_LEN != cmdq_tail) {
		cmdq[cmdq_head].cmd = cmd;
		cmdq[cmdq_head].arg = arg;
		cmdq_head = (cmdq_head + 1) % CMDQ_LEN;
	}
	LWP_MutexUnlock(cmd_lock);
}

void player_get_status(player_status_t *out)
{
	int t, i, n = 0;

	LWP_MutexLock(ring_lock);
	for (i = 0; i < NSLOTS; i++)
		if (slots[i].state != SLOT_FREE)
			n++;
	LWP_MutexUnlock(ring_lock);

	LWP_MutexLock(state_lock);
	*out = st;
	t = heard_track;
	out->buffered = n;
	out->track = (st.play != PLAY_STOPPED) ? t : -1;
	if (out->track >= 0 && out->track < toc.count) {
		cd_track_t *tr = &toc.tracks[out->track];
		out->track_length = tr->length;
		out->track_elapsed = heard_lba > tr->lba ? heard_lba - tr->lba : 0;
	}
	out->read_ms_avg = read_count ? read_ms_total / read_count : 0;
	LWP_MutexUnlock(state_lock);
}

bool player_get_disc(cd_toc_t *otoc, cd_text_t *otxt)
{
	bool ok;
	LWP_MutexLock(state_lock);
	ok = (st.drive == DRIVE_READY || st.drive == DRIVE_NO_AUDIO);
	if (otoc) *otoc = toc;
	if (otxt) *otxt = cdtext;
	LWP_MutexUnlock(state_lock);
	return ok;
}
