/* Disc image reader for the PSX runtime's CD-ROM model: parses a .cue sheet
 * (single file or one .bin per track, as Redump dumps are) and serves sectors
 * from files on the SD card. Implements the iso_* interface from
 * runtime/src/iso_reader_c.cpp. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SEG 100
#define MAX_TRK 100

typedef struct {
    FILE *f;
    uint32_t start;     /* first disc sector in this file */
    uint32_t count;
    int raw;            /* 2352-byte sectors */
    int mode2;          /* MODE2/2352 data: user data starts at byte 24 */
} seg_t;

typedef struct {
    int audio;
    uint32_t start;     /* INDEX 01, disc relative */
    uint32_t pregap;    /* INDEX 00, disc relative (or INDEX 01) */
} trk_t;

typedef struct {
    seg_t seg[MAX_SEG];
    int nseg;
    trk_t trk[MAX_TRK + 1];
    int ntrk;
    uint32_t total;
} disc_t;

static int parse_msf(const char *s, uint32_t *out)
{
    unsigned m, sec, f;
    if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3) return 0;
    *out = (m * 60 + sec) * 75 + f;
    return 1;
}

static void dirname_of(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) + 1 : 0;
    if (len >= n) len = n - 1;
    memcpy(out, path, len);
    out[len] = 0;
}

unsigned g_wii_iso_reads;
unsigned g_wii_iso_fail;

void *iso_open(const char *path)
{
    disc_t *d = (disc_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    size_t plen = strlen(path);
    int is_cue = plen > 4 && strcasecmp(path + plen - 4, ".cue") == 0;

    if (!is_cue) {
        FILE *f = fopen(path, "rb");
        if (!f) { free(d); return NULL; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        d->seg[0].f = f;
        d->seg[0].raw = (sz % 2352) == 0;
        d->seg[0].mode2 = d->seg[0].raw;
        d->seg[0].count = (uint32_t)(sz / (d->seg[0].raw ? 2352 : 2048));
        d->nseg = 1;
        d->trk[1].audio = 0;
        d->ntrk = 1;
        d->total = d->seg[0].count;
        return d;
    }

    FILE *cue = fopen(path, "r");
    if (!cue) { free(d); return NULL; }
    char dir[256];
    dirname_of(path, dir, sizeof(dir));

    char line[512];
    int cur = 0;                 /* current track number, 0 = none */
    uint32_t file_start = 0;     /* disc sector where the current FILE begins */
    int have_idx1[MAX_TRK + 1] = {0};

    while (fgets(line, sizeof(line), cue)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;

        if (!strncasecmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2 || d->nseg >= MAX_SEG) continue;
            *q2 = 0;
            char full[512];
            snprintf(full, sizeof(full), "%s%s", dir, q1 + 1);
            seg_t *s = &d->seg[d->nseg];
            s->f = fopen(full, "rb");
            if (!s->f) { fclose(cue); return NULL; }
            fseek(s->f, 0, SEEK_END);
            long sz = ftell(s->f);
            s->raw = 1;
            s->mode2 = 0;
            s->count = (uint32_t)(sz / 2352);
            s->start = file_start;
            file_start += s->count;
            d->nseg++;
        } else if (!strncasecmp(p, "TRACK", 5)) {
            int n = 0;
            char mode[32] = "";
            if (sscanf(p + 5, "%d %31s", &n, mode) < 1 || n < 1 || n > MAX_TRK) continue;
            cur = n;
            if (n > d->ntrk) d->ntrk = n;
            d->trk[n].audio = !strncasecmp(mode, "AUDIO", 5);
            if (d->nseg > 0 && !d->trk[n].audio &&
                !strncasecmp(mode, "MODE2", 5))
                d->seg[d->nseg - 1].mode2 = 1;
        } else if (!strncasecmp(p, "INDEX", 5) && cur && d->nseg) {
            unsigned idx = 0;
            char msf[32];
            uint32_t t;
            if (sscanf(p + 5, "%u %31s", &idx, msf) != 2 || !parse_msf(msf, &t)) continue;
            uint32_t abs = d->seg[d->nseg - 1].start + t;
            if (idx == 0) {
                d->trk[cur].pregap = abs;
            } else if (idx == 1) {
                d->trk[cur].start = abs;
                have_idx1[cur] = 1;
                if (!d->trk[cur].pregap) d->trk[cur].pregap = abs;
            }
        }
    }
    fclose(cue);

    for (int i = 0; i < d->nseg; i++)
        d->total = d->seg[i].start + d->seg[i].count;
    return d;
}

static seg_t *find_seg(disc_t *d, uint32_t lba)
{
    for (int i = 0; i < d->nseg; i++)
        if (lba >= d->seg[i].start && lba < d->seg[i].start + d->seg[i].count)
            return &d->seg[i];
    return NULL;
}

int iso_read_sector(void *h, uint32_t lba, uint8_t *buf, int size)
{
    (void)size;
    disc_t *d = (disc_t *)h;
    seg_t *s = d ? find_seg(d, lba) : NULL;
    g_wii_iso_reads++;
    if (!s) { g_wii_iso_fail++; return 0; }
    uint32_t rel = lba - s->start;
    long off = s->raw ? (long)rel * 2352 + (s->mode2 ? 24 : 16) : (long)rel * 2048;
    if (fseek(s->f, off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 2048, s->f) == 2048;
}

int iso_read_raw_sector(void *h, uint32_t lba, uint8_t *buf, int size)
{
    disc_t *d = (disc_t *)h;
    seg_t *s = d ? find_seg(d, lba) : NULL;
    g_wii_iso_reads++;
    if (!s || size < 2352 || !s->raw) { g_wii_iso_fail++; return 0; }
    if (fseek(s->f, (long)(lba - s->start) * 2352, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 2352, s->f) == 2352;
}

int iso_read_subq(void *h, uint32_t lba, uint8_t *buf, int size, int *valid)
{
    (void)h; (void)lba; (void)buf; (void)size;
    if (valid) *valid = 0;
    return 0;                       /* no sub-channel data in .bin dumps */
}

int iso_has_subq_replacements(void *h) { (void)h; return 0; }

uint32_t iso_sector_count(void *h) { return h ? ((disc_t *)h)->total : 0; }
int iso_track_count(void *h) { return h ? ((disc_t *)h)->ntrk : 1; }

uint32_t iso_track_start_lba(void *h, int t)
{
    disc_t *d = (disc_t *)h;
    return (d && t >= 1 && t <= d->ntrk) ? d->trk[t].start : 0;
}

uint32_t iso_track_pregap_lba(void *h, int t)
{
    disc_t *d = (disc_t *)h;
    return (d && t >= 1 && t <= d->ntrk) ? d->trk[t].pregap : 0;
}

int iso_track_is_audio(void *h, int t)
{
    disc_t *d = (disc_t *)h;
    return (d && t >= 1 && t <= d->ntrk) ? d->trk[t].audio : 0;
}

void iso_close(void *h)
{
    disc_t *d = (disc_t *)h;
    if (!d) return;
    for (int i = 0; i < d->nseg; i++)
        if (d->seg[i].f) fclose(d->seg[i].f);
    free(d);
}
