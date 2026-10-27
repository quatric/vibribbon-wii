/*
 * netlog - debug log + a tiny TCP shell for poking the drive remotely.
 *
 *   nc <wii-ip> 5555
 *
 * Every log line is streamed to the connected client, and lines typed by the
 * client are run as commands (type "help").
 */
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "netlog.h"
#include "usbcd.h"
#include "cdplayer.h"

#define LOG_LINES     512
#define LOG_LINE_LEN  160

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#define IOS_O_NONBLOCK 0x04

static char log_ring[LOG_LINES][LOG_LINE_LEN];
static u32 log_seq;                 /* total lines ever written */
static mutex_t log_lock = LWP_MUTEX_NULL;
static u64 log_t0;
static FILE *sd_log;
static bool sd_mounted;

static char ip_str[16];
static volatile bool quit;
static volatile bool client_up;
static lwp_t net_thread = LWP_THREAD_NULL;
static s32 client = -1;

void dlog(const char *fmt, ...)
{
	char *line;
	va_list ap;
	u64 ms;
	int n;

	if (log_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&log_lock, false);
	if (!log_t0)
		log_t0 = gettime();
	ms = ticks_to_millisecs(diff_ticks(log_t0, gettime()));

	LWP_MutexLock(log_lock);
	line = log_ring[log_seq % LOG_LINES];
	n = snprintf(line, LOG_LINE_LEN, "[%5u.%03u] ", (u32)(ms / 1000) % 100000, (u32)(ms % 1000));
	va_start(ap, fmt);
	vsnprintf(line + n, LOG_LINE_LEN - n, fmt, ap);
	va_end(ap);
	log_seq++;
	if (sd_log) {
		fputs(line, sd_log);
		fputc('\n', sd_log);
		fflush(sd_log);
	}
	LWP_MutexUnlock(log_lock);
}

int netlog_tail(char lines[][80], int n)
{
	int i, cnt;
	u32 start;

	LWP_MutexLock(log_lock);
	cnt = log_seq < (u32)n ? (int)log_seq : n;
	start = log_seq - cnt;
	for (i = 0; i < cnt; i++) {
		/* skip the timestamp on screen */
		const char *src = log_ring[(start + i) % LOG_LINES];
		const char *p = strchr(src, ']');
		p = p ? p + 2 : src;
		strncpy(lines[i], p, 79);
		lines[i][79] = 0;
	}
	LWP_MutexUnlock(log_lock);
	return cnt;
}

const char *netlog_ip(void) { return ip_str; }
bool netlog_client_connected(void) { return client_up; }

/* ---- client I/O ---- */

static void csend(const char *buf, int len)
{
	u64 start = gettime();
	while (len > 0 && client >= 0) {
		s32 r = net_send(client, buf, len > 1024 ? 1024 : len, 0);
		if (r > 0) {
			buf += r;
			len -= r;
		} else if (r == -EAGAIN || r == 0) {
			if (ticks_to_millisecs(diff_ticks(start, gettime())) > 2000)
				return;
			usleep(2000);
		} else {
			net_close(client);
			client = -1;
			client_up = false;
			return;
		}
	}
}

static void cprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	csend(buf, n);
}

static void hexdump(const u8 *p, u32 len)
{
	u32 i, j;
	for (i = 0; i < len; i += 16) {
		char line[100];
		int n = snprintf(line, sizeof(line), "%04x: ", i);
		for (j = 0; j < 16; j++)
			n += snprintf(line + n, sizeof(line) - n, i + j < len ? "%02x " : "   ", p[i + j]);
		n += snprintf(line + n, sizeof(line) - n, " ");
		for (j = 0; j < 16 && i + j < len; j++)
			line[n++] = (p[i + j] >= 0x20 && p[i + j] < 0x7F) ? p[i + j] : '.';
		line[n++] = '\n';
		csend(line, n);
	}
}

static void print_result(const char *what, int r)
{
	cd_sense_t s = usbcd_last_sense();
	if (r == USBCD_CHECK)
		cprintf("%s: CHECK CONDITION sense %x/%02x/%02x (%s)\n", what, s.key, s.asc, s.ascq, usbcd_sense_str(s));
	else
		cprintf("%s: %s (%d)\n", what, r == 0 ? "OK" : "error", r);
}

static const char *help_text =
	"commands:\n"
	"  status            player/drive status\n"
	"  toc               show table of contents\n"
	"  inquiry | tur     SCSI INQUIRY / TEST UNIT READY\n"
	"  play N | pause | stop | next | prev | seek +S/-S\n"
	"  eject | vol 0-255 | repeat | shuffle | reload\n"
	"  speed KBPS        SET CD SPEED (176=1x, 0=max)\n"
	"  swap              toggle CD-DA byte swap\n"
	"  read LBA [N]      READ CD N raw audio sectors, print stats + first bytes\n"
	"  cdb HEX.. [len=N] [out]  raw SCSI command (data-in unless 'out')\n"
	"  reset | reprobe   BOT reset recovery / close + rediscover drive\n"
	"  log               replay the log buffer\n"
	"  quit              disconnect\n";

static void cmd_status(void)
{
	player_status_t s;
	static const char *dn[] = { "none", "no disc", "tray open", "spin-up", "no audio", "ready" };
	static const char *pn[] = { "stopped", "playing", "paused" };
	player_get_status(&s);
	cprintf("drive: %s [%s] %04x:%04x %s-speed\n", dn[s.drive], s.drive_name, usbcd_vid(), usbcd_pid(),
	        usbcd_highspeed() ? "high" : "full");
	cprintf("play: %s track %d %u:%02u/%u:%02u vol %d repeat %d shuffle %d swap %d\n", pn[s.play],
	        s.track, s.track_elapsed / 75 / 60, (s.track_elapsed / 75) % 60, s.track_length / 75 / 60,
	        (s.track_length / 75) % 60, s.volume, s.repeat, s.shuffle, s.swap);
	cprintf("buffer %d/%d, underruns %u, read errors %u, avg read %u ms/8 sectors, speed %u kB/s\n",
	        s.buffered, s.buffer_slots, s.underruns, s.read_errors, s.read_ms_avg, s.speed);
	cprintf("msg: %s\n", s.status_msg);
}

static void cmd_toc(void)
{
	static cd_toc_t toc;
	static cd_text_t txt;
	int i;
	if (!player_get_disc(&toc, &txt)) {
		cprintf("no disc loaded\n");
		return;
	}
	if (txt.present)
		cprintf("\"%s\" - \"%s\"\n", txt.album, txt.artist);
	for (i = 0; i < toc.count; i++) {
		cd_track_t *t = &toc.tracks[i];
		cprintf("%02d %s ctl %x lba %6u len %6u (%u:%02u) %s\n", t->number, TRACK_IS_DATA(t) ? "data " : "audio",
		        t->control, t->lba, t->length, t->length / 75 / 60, (t->length / 75) % 60, txt.title[t->number - 1]);
	}
	cprintf("leadout %u\n", toc.leadout);
}

static void cmd_read(char *args)
{
	static u8 buf[CD_FRAME_BYTES * USBCD_MAX_READ_SECTORS] ATTRIBUTE_ALIGN(32);
	u32 lba = strtoul(args, &args, 0);
	u32 n = strtoul(args, NULL, 0);
	u32 i, ms;
	int r, peak = 0;
	u64 t0;
	if (n == 0) n = 1;
	if (n > USBCD_MAX_READ_SECTORS) n = USBCD_MAX_READ_SECTORS;
	t0 = gettime();
	r = usbcd_read_audio(lba, n, buf);
	ms = ticks_to_millisecs(diff_ticks(t0, gettime()));
	print_result("READ CD", r);
	if (r != USBCD_OK)
		return;
	for (i = 0; i < n * CD_FRAME_BYTES; i += 2) {
		s16 v = (s16)(buf[i] | (buf[i + 1] << 8));   /* little-endian sample */
		if (abs(v) > peak) peak = abs(v);
	}
	cprintf("%u sectors in %u ms, LE peak %d (%.1f%%)\n", n, ms, peak, peak * 100.0 / 32768);
	hexdump(buf, 64);
}

static void cmd_cdb(char *args)
{
	static u8 buf[CD_FRAME_BYTES * USBCD_MAX_READ_SECTORS] ATTRIBUTE_ALIGN(32);
	u8 cdb[16];
	int clen = 0, r;
	u32 len = 0, got = 0;
	bool out = false;
	char *tok = strtok(args, " ");
	while (tok) {
		if (!strncmp(tok, "len=", 4))
			len = strtoul(tok + 4, NULL, 0);
		else if (!strcmp(tok, "out"))
			out = true;
		else if (clen < 16)
			cdb[clen++] = strtoul(tok, NULL, 16);
		tok = strtok(NULL, " ");
	}
	if (clen == 0) {
		cprintf("usage: cdb 43 00 00 00 00 00 00 03 24 00 len=804\n");
		return;
	}
	if (len > usbcd_bounce_size())
		len = usbcd_bounce_size();
	memset(buf, 0, len);
	r = usbcd_scsi(cdb, clen, buf, len, !out, 15, &got);
	print_result("cdb", r);
	cprintf("%u bytes transferred\n", got);
	if (!out && got)
		hexdump(buf, got > 512 ? 512 : got);
}

static void run_command(char *line)
{
	char *arg;
	int r;

	while (*line == ' ') line++;
	arg = strchr(line, ' ');
	if (arg) {
		*arg++ = 0;
		while (*arg == ' ') arg++;
	} else {
		arg = line + strlen(line);
	}

	if (!*line) return;
	dlog("shell: %s %s", line, arg);

	if (!strcmp(line, "help") || !strcmp(line, "?")) csend(help_text, strlen(help_text));
	else if (!strcmp(line, "status")) cmd_status();
	else if (!strcmp(line, "toc")) cmd_toc();
	else if (!strcmp(line, "inquiry")) {
		u8 t = 0;
		r = usbcd_inquiry(&t);
		print_result("INQUIRY", r);
		if (r == 0) cprintf("type %u: %s\n", t, usbcd_name());
	}
	else if (!strcmp(line, "tur")) print_result("TEST UNIT READY", usbcd_test_unit_ready());
	else if (!strcmp(line, "play")) player_cmd(CMD_PLAY_TRACK, atoi(arg) > 0 ? atoi(arg) - 1 : 0);
	else if (!strcmp(line, "pause")) player_cmd(CMD_TOGGLE_PAUSE, -1);
	else if (!strcmp(line, "stop")) player_cmd(CMD_STOP, 0);
	else if (!strcmp(line, "next")) player_cmd(CMD_NEXT, 0);
	else if (!strcmp(line, "prev")) player_cmd(CMD_PREV, 0);
	else if (!strcmp(line, "seek")) player_cmd(CMD_SEEK, atoi(arg));
	else if (!strcmp(line, "eject")) player_cmd(CMD_EJECT, 0);
	else if (!strcmp(line, "vol")) player_cmd(CMD_VOLUME, atoi(arg));
	else if (!strcmp(line, "repeat")) player_cmd(CMD_REPEAT, 0);
	else if (!strcmp(line, "shuffle")) player_cmd(CMD_SHUFFLE, 0);
	else if (!strcmp(line, "reload")) player_cmd(CMD_RELOAD, 0);
	else if (!strcmp(line, "speed")) player_cmd(CMD_SPEED, atoi(arg));
	else if (!strcmp(line, "swap")) player_cmd(CMD_SWAP, 0);
	else if (!strcmp(line, "read")) cmd_read(arg);
	else if (!strcmp(line, "cdb")) cmd_cdb(arg);
	else if (!strcmp(line, "reset")) print_result("reset", usbcd_reset());
	else if (!strcmp(line, "reprobe")) { usbcd_close(); cprintf("closed; drive thread will rediscover\n"); }
	else if (!strcmp(line, "log")) {
		u32 i, from;
		LWP_MutexLock(log_lock);
		from = log_seq > LOG_LINES ? log_seq - LOG_LINES : 0;
		for (i = from; i < log_seq; i++) {
			cprintf("%s\n", log_ring[i % LOG_LINES]);
		}
		LWP_MutexUnlock(log_lock);
	}
	else if (!strcmp(line, "quit") || !strcmp(line, "exit")) {
		net_close(client);
		client = -1;
		client_up = false;
	}
	else cprintf("unknown command '%s' (try help)\n", line);
}

static void *net_main(void *arg)
{
	char local[16] = "", mask[16] = "", gw[16] = "";
	struct sockaddr_in sa;
	s32 srv;
	u32 sent_seq = 0;
	char inbuf[256];
	int inlen = 0;

	while (!quit) {
		if (if_config(local, mask, gw, true, 5) >= 0 && local[0])
			break;
		dlog("net: no network yet, retrying");
		sleep(5);
	}
	if (quit) return NULL;
	strncpy(ip_str, local, sizeof(ip_str) - 1);
	dlog("net: up at %s, debug shell on port %d", ip_str, NETLOG_PORT);

	srv = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (srv < 0) {
		dlog("net: socket failed %d", srv);
		return NULL;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(NETLOG_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (net_bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 || net_listen(srv, 1) < 0) {
		dlog("net: bind/listen failed");
		net_close(srv);
		return NULL;
	}
	net_fcntl(srv, F_SETFL, net_fcntl(srv, F_GETFL, 0) | IOS_O_NONBLOCK);

	while (!quit) {
		if (client < 0) {
			struct sockaddr_in ca;
			socklen_t cl = sizeof(ca);
			s32 c = net_accept(srv, (struct sockaddr *)&ca, &cl);
			if (c >= 0) {
				client = c;
				net_fcntl(client, F_SETFL, net_fcntl(client, F_GETFL, 0) | IOS_O_NONBLOCK);
				client_up = true;
				inlen = 0;
				cprintf("Wii CD Player debug shell. Type 'help'.\n");
				LWP_MutexLock(log_lock);
				sent_seq = log_seq > LOG_LINES ? log_seq - LOG_LINES : 0;
				LWP_MutexUnlock(log_lock);
				dlog("net: client connected from %s", inet_ntoa(ca.sin_addr));
			} else {
				usleep(100000);
				continue;
			}
		}

		/* stream new log lines */
		for (;;) {
			char line[LOG_LINE_LEN + 2];
			LWP_MutexLock(log_lock);
			if (sent_seq >= log_seq) {
				LWP_MutexUnlock(log_lock);
				break;
			}
			if (log_seq - sent_seq > LOG_LINES)
				sent_seq = log_seq - LOG_LINES;
			snprintf(line, sizeof(line), "%s\n", log_ring[sent_seq % LOG_LINES]);
			sent_seq++;
			LWP_MutexUnlock(log_lock);
			csend(line, strlen(line));
		}
		if (client < 0)
			continue;

		/* read commands */
		{
			s32 r = net_recv(client, inbuf + inlen, sizeof(inbuf) - 1 - inlen, 0);
			if (r > 0) {
				char *nl;
				inlen += r;
				inbuf[inlen] = 0;
				while ((nl = strpbrk(inbuf, "\r\n")) != NULL) {
					*nl = 0;
					run_command(inbuf);
					if (client < 0) break;
					memmove(inbuf, nl + 1, inlen - (nl + 1 - inbuf) + 1);
					inlen = strlen(inbuf);
				}
				if (inlen >= (int)sizeof(inbuf) - 1)
					inlen = 0;
			} else if (r == 0 || (r < 0 && r != -EAGAIN)) {
				dlog("net: client disconnected");
				net_close(client);
				client = -1;
				client_up = false;
			}
		}
		usleep(20000);
	}
	if (client >= 0) net_close(client);
	net_close(srv);
	return NULL;
}

void netlog_init(bool use_sd)
{
	if (log_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&log_lock, false);
	if (use_sd && fatMountSimple("sd", &__io_wiisd)) {
		sd_mounted = true;
		sd_log = fopen("sd:/cdplayer.log", "w");
	}
	LWP_CreateThread(&net_thread, net_main, NULL, NULL, 32 * 1024, 40);
}

void netlog_shutdown(void)
{
	quit = true;
	/* if_config can block for a while; don't wait on the thread if it's stuck there */
	if (ip_str[0] && net_thread != LWP_THREAD_NULL)
		LWP_JoinThread(net_thread, NULL);
	LWP_MutexLock(log_lock);
	if (sd_log) {
		fclose(sd_log);
		sd_log = NULL;
	}
	LWP_MutexUnlock(log_lock);
	if (sd_mounted)
		fatUnmount("sd:");
}
