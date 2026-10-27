/*
 * cdplayer - drive management + CD-DA streaming.
 *
 * Two threads:
 *   drive thread  owns the USB drive: polls for drive/disc, loads the TOC,
 *                 runs commands and reads CD-DA ahead into a ring of slots.
 *   audio thread  turns filled slots into 48 kHz PCM for the audio
 *                 interface's DMA. The AI only plays 32/48 kHz, so the CD's
 *                 44.1 kHz is converted here with a 64-tap polyphase
 *                 windowed-sinc filter (147/160 step), which keeps the
 *                 resampling images out of the audible band. The DMA
 *                 interrupt hands the hardware the next finished buffer.
 */
#include <gccore.h>
#include <ogc/audio.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cdplayer.h"
#include "netlog.h"
#include "metadata.h"

#define SLOT_SECTORS   8                                 /* ~107 ms per slot */
#define SLOT_BYTES     (SLOT_SECTORS * CD_FRAME_BYTES)   /* 18816, multiple of 32 */
#define NSLOTS         32                                /* ~3.4 s read-ahead */
#define PREBUFFER      4
#define DEFAULT_SPEED  706                               /* 4x, quieter than max */

/* resampler: out frame n sits at input position n * RS_IN / RS_OUT */
#define RS_IN          147                               /* 44100 / 300 */
#define RS_OUT         160                               /* 48000 / 300 */
#define TAPS           64
#define IN_CAP         (TAPS + 2048)

/* DMA output ring: one buffer plays, one is latched as next, the rest fill */
#define OUT_FRAMES     1024                              /* ~21 ms at 48 kHz */
#define OUT_BYTES      (OUT_FRAMES * 4)
#define NOUT           6

enum { SLOT_FREE, SLOT_FILLED };

typedef struct {
	u8 *pcm;
	u32 size;                            /* bytes, padded to 32 for the cache flush */
	u32 frames;                          /* stereo frames of audio actually in it */
	u32 lba;
	s16 track;
	volatile u8 state;
} slot_t;

static slot_t slots[NSLOTS];
static u32 w_idx, r_idx, r_off;          /* r_off: frames consumed from slots[r_idx] */
static bool primed, in_underrun, rs_reset;
static volatile bool reader_eof, finished;
static mutex_t ring_lock;

static s32 coef[RS_OUT][TAPS];           /* Q14, each phase sums to 1.0 */
static s16 in_buf[IN_CAP * 2];
static u32 in_len, in_pos, rs_acc;

static s16 out_buf[NOUT][OUT_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static s16 silence[OUT_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static volatile u32 out_filled, out_handed;
static lwpq_t audio_queue = LWP_TQUEUE_NULL;
static u32 tone_phase;

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

static lwp_t drive_thread = LWP_THREAD_NULL, audio_thread = LWP_THREAD_NULL;
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

/* Drop everything buffered. Drive thread only. */
static void ring_flush(void)
{
	int i;
	LWP_MutexLock(ring_lock);
	for (i = 0; i < NSLOTS; i++)
		slots[i].state = SLOT_FREE;
	w_idx = r_idx = r_off = 0;
	primed = false;
	in_underrun = false;
	rs_reset = true;
	reader_eof = false;
	finished = false;
	LWP_MutexUnlock(ring_lock);
}

/* ---- audio thread ---- */

static double bessel_i0(double x)
{
	double sum = 1, term = 1;
	int k;
	for (k = 1; k < 30; k++) {
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
	}
	return sum;
}

/* Lowpass at 21 kHz (passband to ~20 kHz, images from 24 kHz up ~80 dB
 * down), Kaiser window beta 8, sampled at every 1/160 input-sample phase. */
static void make_coefs(void)
{
	const double fc = 21000.0 / 44100.0, beta = 8.0, i0b = bessel_i0(beta);
	int p, k;

	for (p = 0; p < RS_OUT; p++) {
		double h[TAPS], sum = 0;
		s32 isum = 0;
		for (k = 0; k < TAPS; k++) {
			double x = TAPS / 2 - 1 - k + (double)p / RS_OUT;       /* distance from tap to output point */
			double r = x / (TAPS / 2), w = r * r < 1 ? bessel_i0(beta * sqrt(1 - r * r)) / i0b : 0;
			double s = x == 0 ? 1 : sin(M_PI * fc * x) / (M_PI * fc * x);
			h[k] = s * w;
			sum += h[k];
		}
		for (k = 0; k < TAPS; k++) {
			coef[p][k] = (s32)lround(h[k] / sum * 16384);
			isum += coef[p][k];
		}
		coef[p][TAPS / 2 - 1] += 16384 - isum;   /* exact unity gain per phase */
	}
}

/* Move frames from the slot ring into in_buf. Caller holds ring_lock.
 * Returns false when nothing is ready. */
static bool pull_input(void)
{
	slot_t *s = &slots[r_idx];
	const u8 *p;
	u32 n, i;

	if (s->state != SLOT_FILLED)
		return false;
	n = s->frames - r_off;
	if (n > IN_CAP - in_len)
		n = IN_CAP - in_len;
	p = s->pcm + r_off * 4;
	for (i = 0; i < n * 2; i++, p += 2)
		in_buf[in_len * 2 + i] = (s16)(p[0] | p[1] << 8);   /* CD-DA is little-endian */
	in_len += n;
	r_off += n;

	heard_track = s->track;
	heard_lba = s->lba + r_off / (CD_FRAME_BYTES / 4);
	if (r_off >= s->frames) {
		s->state = SLOT_FREE;
		r_idx = (r_idx + 1) % NSLOTS;
		r_off = 0;
	}
	return true;
}

/* 44.1 kHz test signal: 440 Hz on the left, 1 kHz on the right */
static void pull_tone(void)
{
	u32 i, n = IN_CAP - in_len;
	for (i = 0; i < n; i++, tone_phase++) {
		in_buf[(in_len + i) * 2]     = (s16)(sin(2 * M_PI * 440 * (tone_phase % 44100) / 44100.0) * 12000);
		in_buf[(in_len + i) * 2 + 1] = (s16)(sin(2 * M_PI * 1000 * (tone_phase % 44100) / 44100.0) * 12000);
	}
	in_len += n;
}

static int slots_ready(void)
{
	int n = 0;
	while (n < NSLOTS && slots[(r_idx + n) % NSLOTS].state == SLOT_FILLED)
		n++;
	return n;
}

/* Render one DMA buffer: big-endian s16 stereo, and the AI wants the
 * right channel first. */
static void render(s16 *out)
{
	play_state_t ps;
	bool tone;
	int vol, n = 0;

	LWP_MutexLock(state_lock);
	ps = st.play;
	vol = st.volume;
	tone = st.tone;
	LWP_MutexUnlock(state_lock);

	LWP_MutexLock(ring_lock);
	if (rs_reset) {
		/* start each stream from silence so the filter has history */
		memset(in_buf, 0, (TAPS - 1) * 4);
		in_len = TAPS - 1;
		in_pos = rs_acc = 0;
		rs_reset = false;
	}
	if (!tone && ps == PLAY_PLAYING && !primed && (slots_ready() >= PREBUFFER || (reader_eof && slots_ready())))
		primed = true;

	if (tone || (ps == PLAY_PLAYING && primed)) {
		for (; n < OUT_FRAMES; n++) {
			const s16 *x;
			const s32 *c;
			s32 l = 0, r = 0, k;

			if (in_pos + TAPS > in_len) {
				memmove(in_buf, in_buf + in_pos * 2, (in_len - in_pos) * 4);
				in_len -= in_pos;
				in_pos = 0;
				if (tone)
					pull_tone();
				else
					while (in_len < IN_CAP && pull_input())
						;
				if (in_pos + TAPS > in_len)
					break;
			}
			x = in_buf + in_pos * 2;
			c = coef[rs_acc];
			for (k = 0; k < TAPS; k++, x += 2) {
				l += x[0] * c[k];
				r += x[1] * c[k];
			}
			l = ((l >> 14) * vol) / 255;
			r = ((r >> 14) * vol) / 255;
			out[n * 2]     = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
			out[n * 2 + 1] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;

			rs_acc += RS_IN;
			if (rs_acc >= RS_OUT) {
				rs_acc -= RS_OUT;
				in_pos++;
			}
		}
		if (n < OUT_FRAMES && !tone) {
			if (reader_eof && slots_ready() == 0) {
				finished = true;
			} else if (!in_underrun) {
				in_underrun = true;
				st.underruns++;
				dlog("audio: buffer underrun (%u)", st.underruns);
			}
		} else {
			in_underrun = false;
		}
	}
	LWP_MutexUnlock(ring_lock);

	if (n < OUT_FRAMES)
		memset(out + n * 2, 0, (OUT_FRAMES - n) * 4);
	DCFlushRange(out, OUT_BYTES);
}

/* AI DMA interrupt: the block queued last time just started playing, so
 * queue the next one now. */
static void dma_cb(void)
{
	if (out_handed != out_filled) {
		AUDIO_InitDMA((u32)out_buf[out_handed % NOUT], OUT_BYTES);
		out_handed++;
	} else {
		AUDIO_InitDMA((u32)silence, OUT_BYTES);
	}
	LWP_ThreadSignal(audio_queue);
}

static void *audio_main(void *arg)
{
	while (!quit) {
		/* stay clear of the playing and the latched buffer */
		while (out_filled - out_handed < NOUT - 2) {
			render(out_buf[out_filled % NOUT]);
			out_filled++;
		}
		LWP_ThreadSleep(audio_queue);
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
		} else {
			set_play(st.play == PLAY_PLAYING ? PLAY_PAUSED : PLAY_PLAYING);
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
	case CMD_TONE:
		LWP_MutexLock(state_lock);
		st.tone = !st.tone;
		LWP_MutexUnlock(state_lock);
		LWP_MutexLock(ring_lock);
		rs_reset = true;
		LWP_MutexUnlock(ring_lock);
		dlog("audio: test tone %s", st.tone ? "on (440 Hz left, 1 kHz right)" : "off");
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

	s->frames = n * CD_FRAME_BYTES / 4;
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

	make_coefs();
	rs_reset = true;
	DCFlushRange(silence, OUT_BYTES);
	LWP_InitQueue(&audio_queue);
	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(dma_cb);
	AUDIO_InitDMA((u32)silence, OUT_BYTES);
	AUDIO_StartDMA();

	LWP_CreateThread(&audio_thread, audio_main, NULL, NULL, 16 * 1024, 100);
	LWP_CreateThread(&drive_thread, drive_main, NULL, NULL, 64 * 1024, 70);
}

void player_shutdown(void)
{
	quit = true;
	if (drive_thread != LWP_THREAD_NULL)
		LWP_JoinThread(drive_thread, NULL);
	if (audio_thread != LWP_THREAD_NULL) {
		LWP_ThreadSignal(audio_queue);
		LWP_JoinThread(audio_thread, NULL);
	}
	AUDIO_StopDMA();
	AUDIO_RegisterDMACallback(NULL);
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
