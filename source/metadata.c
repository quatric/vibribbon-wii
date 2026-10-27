/*
 * metadata - album/track names from MusicBrainz, looked up by disc ID with
 * HTTPS (mbedTLS) and cached on SD so a disc only ever needs one network request.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "metadata.h"
#include "netlog.h"
#include "json.h"
#include "sha1.h"
#include "http.h"

#define MB_HOST        "https://musicbrainz.org"
#define USER_AGENT     "WiiCD/0.1.0 ( https://github.com/quatric/WiiCD )"
#define CACHE_DIR      "sd:/apps/cdplayer/cache"
#define MAX_BODY       (4 * 1024 * 1024)

static mutex_t lock = LWP_MUTEX_NULL;
static lwp_t thread = LWP_THREAD_NULL;
static volatile bool quit;

/* request (guarded by lock) */
static bool req_pending, req_force;
static cd_toc_t req_toc;
static u32 req_gen;

/* result (guarded by lock) */
static meta_state_t cur_state = META_NONE;
static cd_text_t cur_txt;
static char cur_discid[DISCID_LEN];
static char cur_source[96];

static u64 last_request;

const char *meta_state_str(meta_state_t s)
{
	switch (s) {
	case META_NONE:      return "";
	case META_WAIT_NET:  return "waiting for network";
	case META_LOOKUP:    return "looking up...";
	case META_FOUND:     return "MusicBrainz";
	case META_CACHED:    return "MusicBrainz (cached)";
	case META_NOT_FOUND: return "not in MusicBrainz";
	case META_ERROR:     return "lookup failed";
	}
	return "";
}

/* ---- disc ID ---- */

void meta_discid(int first, int last, u32 leadout, const u32 *offsets, char out[DISCID_LEN])
{
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
	char buf[2 + 2 + 100 * 8 + 1];
	u8 h[20];
	int i, n = 0, o = 0;

	n += sprintf(buf + n, "%02X%02X%08X", first, last, leadout);
	for (i = 1; i < 100; i++)
		n += sprintf(buf + n, "%08X", (i >= first && i <= last) ? offsets[i - first] : 0);
	sha1_digest(buf, n, h);

	/* base64 with MusicBrainz's URL-safe alphabet ('.', '_', '-' padding) */
	for (i = 0; i < 18; i += 3) {
		u32 v = (h[i] << 16) | (h[i + 1] << 8) | h[i + 2];
		out[o++] = b64[(v >> 18) & 63];
		out[o++] = b64[(v >> 12) & 63];
		out[o++] = b64[(v >> 6) & 63];
		out[o++] = b64[v & 63];
	}
	{
		u32 v = (h[18] << 16) | (h[19] << 8);
		out[o++] = b64[(v >> 18) & 63];
		out[o++] = b64[(v >> 12) & 63];
		out[o++] = b64[(v >> 6) & 63];
		out[o++] = '-';
	}
	out[o] = 0;
}

/* MusicBrainz identifies the audio session: tracks from the first one up to
 * the last audio track, with the lead-out at the end of that audio track. */
static bool toc_params(const cd_toc_t *toc, int *first, int *last, u32 *leadout, u32 offsets[CD_MAX_TRACKS])
{
	int i, la = -1;

	for (i = 0; i < toc->count; i++)
		if (!TRACK_IS_DATA(&toc->tracks[i]))
			la = i;
	if (la < 0)
		return false;

	*first = toc->tracks[0].number;
	*last = toc->tracks[la].number;
	*leadout = toc->tracks[la].lba + toc->tracks[la].length + 150;
	for (i = 0; i <= la; i++)
		offsets[i] = toc->tracks[i].lba + 150;
	return true;
}

/* ---- text helpers ---- */

/* The console font is ASCII-only: transliterate what we reasonably can. */
static void to_ascii(const char *in, char *out, int cap)
{
	static const char latin1[] =
		"AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
	const u8 *p = (const u8 *)in;
	int o = 0;

	while (*p && o < cap - 1) {
		u32 cp;
		int len;

		if (*p < 0x80) { cp = *p; len = 1; }
		else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
		else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
		else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x10000; len = 4; }
		else { cp = '?'; len = 1; }
		p += len;

		if (cp >= 0x20 && cp < 0x7F) out[o++] = cp;
		else if (cp >= 0xC0 && cp <= 0xFF) out[o++] = latin1[cp - 0xC0];
		else if (cp == 0xA0 || cp == '\t') out[o++] = ' ';
		else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032 || cp == 0xB4) out[o++] = '\'';
		else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) out[o++] = '"';
		else if (cp >= 0x2010 && cp <= 0x2015) out[o++] = '-';
		else if (cp == 0x2026 && o < cap - 3) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
		else if (cp >= 0x20) out[o++] = '?';
	}
	out[o] = 0;
}

static void credit_str(const jnode *ac, char *out, int cap)
{
	char tmp[256] = "";
	int n = 0;
	const jnode *c;

	if (ac && ac->type == J_ARR) {
		for (c = ac->child; c; c = c->next)
			n += snprintf(tmp + n, sizeof(tmp) - n > 0 ? sizeof(tmp) - n : 0, "%s%s",
			              json_str(c, "name"), json_str(c, "joinphrase"));
	}
	to_ascii(tmp, out, cap);
}

/* ---- HTTP ---- */

/* GET url; returns HTTP status (or -1 on transport error); *out must be freed */
static long mb_get(const char *url, char **out)
{
	char err[128];
	size_t len;
	u64 since;
	int code;

	/* MusicBrainz asks for at most one request per second */
	since = ticks_to_millisecs(diff_ticks(last_request, gettime()));
	if (last_request && since < 1100)
		usleep((1100 - since) * 1000);
	last_request = gettime();

	code = http_get(url, USER_AGENT, MAX_BODY, out, &len, err, sizeof(err));
	if (code < 0)
		dlog("meta: %s", err);
	dlog("meta: GET %s -> %d (%u bytes)", url + strlen(MB_HOST), code, (u32)len);
	return code;
}

/* ---- MusicBrainz response handling ---- */

static void fill_from_medium(const jnode *release, const jnode *medium, int first, cd_text_t *txt)
{
	const jnode *tracks = json_get(medium, "tracks"), *t;
	int i = 0;

	memset(txt, 0, sizeof(*txt));
	to_ascii(json_str(release, "title"), txt->album, sizeof(txt->album));
	credit_str(json_get(release, "artist-credit"), txt->artist, sizeof(txt->artist));

	for (t = tracks ? tracks->child : NULL; t; t = t->next, i++) {
		int num = first + (int)json_num(t, "position", i + 1) - 1;
		const char *title = json_str(t, "title");
		if (!title[0])
			title = json_str(json_get(t, "recording"), "title");
		if (num < 1 || num > CD_MAX_TRACKS)
			continue;
		to_ascii(title, txt->title[num - 1], sizeof(txt->title[0]));
		credit_str(json_get(t, "artist-credit"), txt->performer[num - 1], sizeof(txt->performer[0]));
		if (!strcmp(txt->performer[num - 1], txt->artist))
			txt->performer[num - 1][0] = 0;   /* only show per-track artists that differ */
	}
	txt->present = true;
}

/* How far a MusicBrainz disc's TOC is from ours (sum of sector differences),
 * or -1 if the track counts differ. */
static s64 toc_distance(const jnode *disc, int ntracks, u32 leadout, const u32 *offsets)
{
	const jnode *offs = json_get(disc, "offsets"), *o;
	s64 d;
	int i = 0;

	if (json_len(offs) != ntracks)
		return -1;
	d = (s64)json_num(disc, "sectors", 0) - leadout;
	if (d < 0) d = -d;
	for (o = offs->child; o; o = o->next, i++) {
		s64 x = (s64)o->num - offsets[i];
		d += x < 0 ? -x : x;
	}
	return d;
}

/* Picks the medium carrying our disc ID; failing that (fuzzy TOC match) the
 * medium with the closest TOC; then one with the right track count; then the
 * first. Multi-disc sets often have discs with equal track counts, so the
 * TOC comparison matters. */
static const jnode *pick_medium(const jnode *releases, const char *discid, int ntracks, u32 leadout,
                                const u32 *offsets, const jnode **rel_out)
{
	const jnode *r, *m, *d, *best = NULL, *best_rel = NULL;
	s64 best_dist = -1;
	int pass;

	for (pass = 0; pass < 4 && !best; pass++) {
		for (r = releases ? releases->child : NULL; r && (pass == 1 || !best); r = r->next) {
			const jnode *media = json_get(r, "media");
			for (m = media ? media->child : NULL; m && (pass == 1 || !best); m = m->next) {
				const jnode *discs = json_get(m, "discs");
				if (pass == 0) {
					for (d = discs ? discs->child : NULL; d; d = d->next)
						if (!strcmp(json_str(d, "id"), discid))
							break;
					if (!d) continue;
				} else if (pass == 1) {
					for (d = discs ? discs->child : NULL; d; d = d->next) {
						s64 dist = toc_distance(d, ntracks, leadout, offsets);
						if (dist >= 0 && (best_dist < 0 || dist < best_dist)) {
							best_dist = dist;
							best = m;
							best_rel = r;
						}
					}
					continue;
				} else if (pass == 2 && (int)json_num(m, "track-count", -1) != ntracks) {
					continue;
				}
				best = m;
				best_rel = r;
			}
		}
	}
	*rel_out = best_rel;
	return best;
}

meta_state_t meta_lookup(int first, int last, u32 leadout, const u32 *offsets, cd_text_t *txt,
                         char *source, int source_len)
{
	char discid[DISCID_LEN];
	char url[1400];
	char *body = NULL;
	jnode *root = NULL, *rel_root = NULL;
	const jnode *release, *medium;
	long code;
	int i, n, attempt;
	meta_state_t result = META_ERROR;

	meta_discid(first, last, leadout, offsets, discid);
	n = snprintf(url, sizeof(url), MB_HOST "/ws/2/discid/%s?fmt=json&inc=artist-credits+recordings&toc=%d+%d+%u",
	             discid, first, last, leadout);
	for (i = 0; i <= last - first; i++)
		n += snprintf(url + n, sizeof(url) - n, "+%u", offsets[i]);

	for (attempt = 0; attempt < 3; attempt++) {
		code = mb_get(url, &body);
		if (code != 503)
			break;
		free(body);
		body = NULL;
		sleep(2);
	}
	if (code == 404) {
		result = META_NOT_FOUND;
		goto out;
	}
	if (code != 200 || !body || !(root = json_parse(body))) {
		dlog("meta: bad response (%ld)", code);
		goto out;
	}

	medium = pick_medium(json_get(root, "releases"), discid, last - first + 1, leadout, offsets, &release);
	if (!medium) {
		result = META_NOT_FOUND;
		goto out;
	}

	/* fuzzy TOC matches can come back without track lists */
	if (!json_get(medium, "tracks")) {
		char *body2 = NULL;
		int pos = (int)json_num(medium, "position", 1);
		snprintf(url, sizeof(url), MB_HOST "/ws/2/release/%s?fmt=json&inc=artist-credits+recordings",
		         json_str(release, "id"));
		if (mb_get(url, &body2) == 200 && body2 && (rel_root = json_parse(body2))) {
			const jnode *media = json_get(rel_root, "media"), *m;
			release = rel_root;
			for (m = media ? media->child : NULL; m; m = m->next)
				if ((int)json_num(m, "position", 0) == pos)
					break;
			medium = m ? m : json_at(media, 0);
		}
		/* body2 must outlive rel_root; keep it with the tree */
		if (!rel_root)
			free(body2);
		else
			rel_root->str = body2;
		if (!medium)
			goto out;
	}

	fill_from_medium(release, medium, first, txt);
	snprintf(source, source_len, "release %s", json_str(release, "id"));
	dlog("meta: %s - %s (%s)", txt->artist, txt->album, json_str(release, "id"));
	result = META_FOUND;

out:
	if (rel_root) {
		free(rel_root->str);
		rel_root->str = NULL;
		json_free(rel_root);
	}
	json_free(root);
	free(body);
	return result;
}

/* ---- SD cache ---- */

static void cache_path(const char *discid, char *out, int len)
{
	snprintf(out, len, CACHE_DIR "/%s.txt", discid);
}

static bool cache_load(const char *discid, cd_text_t *txt, char *source, int source_len)
{
	char path[96], line[256];
	FILE *f;

	cache_path(discid, path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return false;
	memset(txt, 0, sizeof(*txt));
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		int t;
		if (nl) *nl = 0;
		if (!eq) continue;
		*eq++ = 0;
		if (!strcmp(line, "album")) snprintf(txt->album, sizeof(txt->album), "%s", eq);
		else if (!strcmp(line, "artist")) snprintf(txt->artist, sizeof(txt->artist), "%s", eq);
		else if (!strcmp(line, "source")) snprintf(source, source_len, "%s", eq);
		else if (sscanf(line, "title.%d", &t) == 1 && t >= 1 && t <= CD_MAX_TRACKS)
			snprintf(txt->title[t - 1], sizeof(txt->title[0]), "%s", eq);
		else if (sscanf(line, "performer.%d", &t) == 1 && t >= 1 && t <= CD_MAX_TRACKS)
			snprintf(txt->performer[t - 1], sizeof(txt->performer[0]), "%s", eq);
	}
	fclose(f);
	txt->present = true;
	return true;
}

static void cache_save(const char *discid, const cd_text_t *txt, const char *source)
{
	char path[96];
	FILE *f;
	int t;

	mkdir("sd:/apps", 0777);
	mkdir("sd:/apps/cdplayer", 0777);
	mkdir(CACHE_DIR, 0777);
	cache_path(discid, path, sizeof(path));
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "source=%s\nalbum=%s\nartist=%s\n", source, txt->album, txt->artist);
	for (t = 0; t < CD_MAX_TRACKS; t++) {
		if (txt->title[t][0]) fprintf(f, "title.%d=%s\n", t + 1, txt->title[t]);
		if (txt->performer[t][0]) fprintf(f, "performer.%d=%s\n", t + 1, txt->performer[t]);
	}
	fclose(f);
	dlog("meta: cached as %s", path);
}

/* ---- worker ---- */

static void set_result(u32 gen, meta_state_t s, const cd_text_t *txt, const char *source)
{
	LWP_MutexLock(lock);
	if (gen == req_gen) {
		cur_state = s;
		if (txt) cur_txt = *txt;
		if (source) snprintf(cur_source, sizeof(cur_source), "%s", source);
	}
	LWP_MutexUnlock(lock);
}

static void *meta_main(void *arg)
{
	static cd_toc_t toc;
	static cd_text_t txt;
	char source[96];

	while (!quit) {
		int first, last;
		u32 leadout, offsets[CD_MAX_TRACKS], gen;
		bool force;
		char discid[DISCID_LEN];
		meta_state_t r;

		LWP_MutexLock(lock);
		if (!req_pending) {
			LWP_MutexUnlock(lock);
			usleep(100000);
			continue;
		}
		req_pending = false;
		toc = req_toc;
		force = req_force;
		gen = req_gen;
		LWP_MutexUnlock(lock);

		if (!toc_params(&toc, &first, &last, &leadout, offsets)) {
			set_result(gen, META_NONE, NULL, "");
			continue;
		}
		meta_discid(first, last, leadout, offsets, discid);
		LWP_MutexLock(lock);
		if (gen == req_gen)
			strcpy(cur_discid, discid);
		LWP_MutexUnlock(lock);
		dlog("meta: disc id %s", discid);

		source[0] = 0;
		if (!force && cache_load(discid, &txt, source, sizeof(source))) {
			dlog("meta: cache hit: %s - %s", txt.artist, txt.album);
			set_result(gen, META_CACHED, &txt, source);
			continue;
		}

		set_result(gen, META_WAIT_NET, NULL, NULL);
		while (!quit && !netlog_ip()[0] && gen == req_gen)
			usleep(250000);
		if (quit || gen != req_gen)
			continue;

		set_result(gen, META_LOOKUP, NULL, NULL);
		r = meta_lookup(first, last, leadout, offsets, &txt, source, sizeof(source));
		set_result(gen, r, r == META_FOUND ? &txt : NULL, r == META_FOUND ? source : "");
		if (r == META_FOUND)
			cache_save(discid, &txt, source);
	}
	return NULL;
}

void meta_init(void)
{
	LWP_MutexInit(&lock, false);
	LWP_CreateThread(&thread, meta_main, NULL, NULL, 128 * 1024, 30);
}

void meta_shutdown(void)
{
	/* a lookup in flight finishes within the HTTP timeout; don't block exit on it */
	quit = true;
}

void meta_request(const cd_toc_t *toc)
{
	LWP_MutexLock(lock);
	req_gen++;
	memset(&cur_txt, 0, sizeof(cur_txt));
	cur_discid[0] = 0;
	cur_source[0] = 0;
	if (toc) {
		req_toc = *toc;
		req_pending = true;
		req_force = false;
		cur_state = META_LOOKUP;
	} else {
		req_pending = false;
		cur_state = META_NONE;
	}
	LWP_MutexUnlock(lock);
}

void meta_refresh(void)
{
	LWP_MutexLock(lock);
	if (req_toc.count > 0 && cur_state != META_NONE) {
		req_gen++;
		req_pending = true;
		req_force = true;
		cur_state = META_LOOKUP;
	}
	LWP_MutexUnlock(lock);
}

meta_state_t meta_get(cd_text_t *txt, char discid[DISCID_LEN], char *source, int source_len)
{
	meta_state_t s;
	LWP_MutexLock(lock);
	s = cur_state;
	if (txt) *txt = cur_txt;
	if (discid) strcpy(discid, cur_discid);
	if (source) snprintf(source, source_len, "%s", cur_source);
	LWP_MutexUnlock(lock);
	return s;
}
