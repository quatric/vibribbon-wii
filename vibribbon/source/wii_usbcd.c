/* A USB CD/DVD drive as the PlayStation's CD drive for your own music CDs
 * (Vib-Ribbon builds courses from them). The drive is WiiCD's usbcd.c.
 *
 * A thread owns the drive: it finds it, watches for a disc going in or coming
 * out, reads the TOC, and reads ahead of where the emulated drive is reading.
 * The emulated drive only copies sectors out of that cache, and waits only
 * when it reads somewhere the thread has not been yet (after a seek).
 *
 * A music CD put in the USB drive is swapped in as if the PlayStation's lid
 * was opened and closed with it; taking it out swaps the game disc back. A CD
 * already in the drive when the game starts is left alone until it is taken
 * out and put back, so the game boots from its own disc. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/cond.h>

#include "usbcd.h"
#include "wii_host.h"

#define CHUNK     USBCD_MAX_READ_SECTORS   /* sectors per drive read */
#define NCHUNK    32                        /* 1.2 MB, 6.8 s of audio */
#define AHEAD     (16 * CHUNK)              /* read this far past the last request */

typedef struct {
    uint32_t lba;      /* first sector */
    uint32_t count;    /* sectors held, 0 if empty */
    uint32_t used;     /* last use, for replacement */
    uint8_t *data;
} chunk_t;

static mutex_t s_lock = LWP_MUTEX_NULL;
static cond_t s_cond = LWP_COND_NULL;
static lwp_t s_thread = LWP_THREAD_NULL;
static chunk_t s_chunk[NCHUNK];
static uint32_t s_clock;

static cd_toc_t s_toc;              /* valid while s_ready */
static volatile int s_ready;        /* an audio disc is in and its TOC read */
static volatile int s_stop, s_stopped;   /* shutting down */
static volatile int s_event;        /* WII_USBCD_INSERTED / _REMOVED for the frame hook */
static uint32_t s_gen;              /* bumped per disc: stale reads are dropped */

static uint32_t s_want = UINT32_MAX;   /* sector the emulated drive waits for */
static int s_want_failed;
static uint32_t s_last = UINT32_MAX;   /* last sector served: read ahead from here */

static chunk_t *find(uint32_t lba)
{
    for (int i = 0; i < NCHUNK; i++)
        if (s_chunk[i].count && lba - s_chunk[i].lba < s_chunk[i].count)
            return &s_chunk[i];
    return NULL;
}

static void drop_cache(void)
{
    for (int i = 0; i < NCHUNK; i++) s_chunk[i].count = 0;
    s_want = UINT32_MAX;
    s_last = UINT32_MAX;
}

/* The next sector to fetch, or UINT32_MAX: a waited-for sector first, then
 * the first one missing ahead of the last request. Called locked. */
static uint32_t next_fetch(void)
{
    if (s_want != UINT32_MAX) return s_want;
    if (s_last == UINT32_MAX) return UINT32_MAX;
    for (uint32_t l = s_last; l < s_last + AHEAD && l < s_toc.leadout; ) {
        chunk_t *c = find(l);
        if (!c) return l;
        l = c->lba + c->count;
    }
    return UINT32_MAX;
}

/* Read up to CHUNK sectors at lba into the least recently used chunk that
 * holds nothing at or after the last request's neighbourhood. */
static int fetch(uint32_t lba, uint32_t gen)
{
    static uint8_t buf[CHUNK * CD_FRAME_BYTES] __attribute__((aligned(32)));
    uint32_t n = CHUNK;
    if (lba >= s_toc.leadout) return USBCD_EINVAL;
    if (n > s_toc.leadout - lba) n = s_toc.leadout - lba;
    LWP_MutexLock(s_lock);
    for (uint32_t i = 1; i < n; i++)          /* stop before what is already cached */
        if (find(lba + i)) { n = i; break; }
    LWP_MutexUnlock(s_lock);
    int r = usbcd_read_audio(lba, n, buf);
    LWP_MutexLock(s_lock);
    if (gen == s_gen) {
        if (r == USBCD_OK) {
            chunk_t *v = &s_chunk[0];
            for (int i = 1; i < NCHUNK; i++)
                if (!s_chunk[i].count || (v->count && s_chunk[i].used < v->used)) v = &s_chunk[i];
            memcpy(v->data, buf, n * CD_FRAME_BYTES);
            v->lba = lba;
            v->count = n;
            v->used = ++s_clock;
        } else if (s_want == lba) {
            s_want_failed = 1;
        }
        LWP_CondBroadcast(s_cond);
    }
    LWP_MutexUnlock(s_lock);
    return r;
}

static int disc_absent(void)
{
    cd_sense_t s = usbcd_last_sense();
    return s.key == 0x02 && s.asc == 0x3A;
}

static void set_ready(int ready)
{
    LWP_MutexLock(s_lock);
    s_gen++;
    drop_cache();
    s_ready = ready;
    s_event = ready ? WII_USBCD_INSERTED : WII_USBCD_REMOVED;
    s_want_failed = 1;
    LWP_CondBroadcast(s_cond);
    LWP_MutexUnlock(s_lock);
}

static void *drive_thread(void *arg)
{
    (void)arg;
    int armed = 0;        /* the drive was seen without a disc: the next disc counts */
    int found = 0;
    while (!s_stop) {
        if (!usbcd_is_open()) {
            if (s_ready) set_ready(0);
            found = 0;
            if (!usbcd_probe()) { usleep(500 * 1000); continue; }
            wii_log("usbcd: %s\n", usbcd_name());
        }
        if (!s_ready) {
            int r = usbcd_test_unit_ready();
            if (r < 0) { usbcd_close(); continue; }
            if (r != USBCD_OK) {
                cd_sense_t s = usbcd_last_sense();
                if (disc_absent() || s.key == 0x06) armed = 1;
                found = 1;
                usleep(500 * 1000);
                continue;
            }
            if (!found) {         /* a disc was already in when the drive turned up */
                found = 1;
                wii_log("usbcd: disc already in; take it out and put it back to use it\n");
            }
            if (!armed) { usleep(500 * 1000); continue; }
            cd_toc_t toc;
            if (usbcd_read_toc(&toc) != USBCD_OK || toc.audio_count == 0) {
                wii_log("usbcd: no audio tracks on this disc\n");
                armed = 0;
                continue;
            }
            LWP_MutexLock(s_lock);
            s_toc = toc;
            LWP_MutexUnlock(s_lock);
            wii_log("usbcd: music CD in, %d tracks, %u sectors\n", toc.count, (unsigned)toc.leadout);
            set_ready(1);
            armed = 0;
            continue;
        }

        LWP_MutexLock(s_lock);
        uint32_t lba = next_fetch(), gen = s_gen;
        if (lba == UINT32_MAX) {
            struct timespec ts = { 0, 500 * 1000 * 1000 };
            LWP_CondTimedWait(s_cond, s_lock, &ts);
            LWP_MutexUnlock(s_lock);
            /* idle: is the disc still there? */
            if (usbcd_test_unit_ready() != USBCD_OK && (disc_absent() || usbcd_last_sense().key == 0x06 ||
                                                         !usbcd_is_open())) {
                wii_log("usbcd: music CD out\n");
                set_ready(0);
                armed = 1;
            }
            continue;
        }
        LWP_MutexUnlock(s_lock);
        int r = fetch(lba, gen);
        if (r != USBCD_OK && (r == USBCD_ENODEV || disc_absent())) {
            wii_log("usbcd: music CD out\n");
            set_ready(0);
            armed = 1;
        }
    }
    s_stopped = 1;
    return NULL;
}

/* usbcd.c logs through WiiCD's network logger; here it goes to the SD log. */
void dlog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wii_log("usbcd: %s\n", line);
}

void wii_usbcd_init(void)
{
    if (usbcd_init() < 0) {
        wii_log("usbcd: no room for the USB buffers\n");
        return;
    }
    for (int i = 0; i < NCHUNK; i++) {
        s_chunk[i].data = (uint8_t *)memalign(32, CHUNK * CD_FRAME_BYTES);
        if (!s_chunk[i].data) { wii_log("usbcd: out of memory\n"); return; }
    }
    LWP_MutexInit(&s_lock, false);
    LWP_CondInit(&s_cond);
    LWP_CreateThread(&s_thread, drive_thread, NULL, NULL, 16384, 70);   /* above the guest's 64 */
}

/* Before leaving: stop the drive thread between transfers, then close the
 * drive (a transfer in flight finishes or times out first). */
void wii_usbcd_shutdown(void)
{
    if (s_thread == LWP_THREAD_NULL) return;
    s_stop = 1;
    LWP_MutexLock(s_lock);
    LWP_CondBroadcast(s_cond);
    LWP_MutexUnlock(s_lock);
    for (int i = 0; i < 100 && !s_stopped; i++) usleep(10000);
    usbcd_close();
}

int wii_usbcd_take_event(void)
{
    if (s_lock == LWP_MUTEX_NULL) return 0;
    LWP_MutexLock(s_lock);
    int e = s_event;
    s_event = 0;
    LWP_MutexUnlock(s_lock);
    return e;
}

int wii_usbcd_toc(cd_toc_t *out)
{
    LWP_MutexLock(s_lock);
    int ok = s_ready;
    if (ok) *out = s_toc;
    LWP_MutexUnlock(s_lock);
    return ok;
}

/* One 2352-byte sector for the emulated drive; waits (up to 10 s) if the
 * thread has not read it yet. */
int wii_usbcd_read(uint32_t lba, uint8_t *out)
{
    if (s_lock == LWP_MUTEX_NULL) return 0;
    LWP_MutexLock(s_lock);
    uint32_t gen = s_gen;
    int ok = 0;
    for (int tries = 0; s_ready && gen == s_gen && tries < 20; tries++) {
        chunk_t *c = find(lba);
        if (c) {
            memcpy(out, c->data + (lba - c->lba) * CD_FRAME_BYTES, CD_FRAME_BYTES);
            c->used = ++s_clock;
            s_last = lba;
            if (s_want == lba) s_want = UINT32_MAX;
            LWP_CondBroadcast(s_cond);        /* read further ahead */
            ok = 1;
            break;
        }
        s_want = lba;
        s_want_failed = 0;
        LWP_CondBroadcast(s_cond);
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        LWP_CondTimedWait(s_cond, s_lock, &ts);
        if (s_want_failed) break;
    }
    if (!ok && s_want == lba) s_want = UINT32_MAX;
    LWP_MutexUnlock(s_lock);
    return ok;
}
