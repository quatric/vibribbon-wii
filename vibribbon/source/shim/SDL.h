/* Minimal SDL2 surface for the Wii build, on libogc. Only what the PSX runtime
 * sources we compile actually call: threads, mutexes, conditions, atomics and
 * the timebase. */
#ifndef WII_SDL_SHIM_H
#define WII_SDL_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/cond.h>
#include <ogc/lwp_watchdog.h>

#define SDL_MUTEX_TIMEDOUT 1
#define SDL_THREAD_PRIORITY_LOW 0

typedef struct { mutex_t m; } SDL_mutex;
typedef struct { cond_t c; } SDL_cond;
typedef struct { lwp_t t; } SDL_Thread;
typedef struct { volatile int value; } SDL_atomic_t;
typedef int (*SDL_ThreadFunction)(void *);

static inline SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *m = (SDL_mutex *)calloc(1, sizeof(*m));
    if (m) LWP_MutexInit(&m->m, true);
    return m;
}
static inline int SDL_LockMutex(SDL_mutex *m) { return LWP_MutexLock(m->m); }
static inline int SDL_UnlockMutex(SDL_mutex *m) { return LWP_MutexUnlock(m->m); }

static inline SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *c = (SDL_cond *)calloc(1, sizeof(*c));
    if (c) LWP_CondInit(&c->c);
    return c;
}
static inline int SDL_CondSignal(SDL_cond *c) { return LWP_CondSignal(c->c); }
static inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { return LWP_CondWait(c->c, m->m); }
static inline int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    return LWP_CondTimedWait(c->c, m->m, &ts) == 0 ? 0 : SDL_MUTEX_TIMEDOUT;
}

struct wii_thread_args { SDL_ThreadFunction fn; void *data; };
static inline void *wii_thread_tramp(void *p)
{
    struct wii_thread_args a = *(struct wii_thread_args *)p;
    free(p);
    return (void *)(intptr_t)a.fn(a.data);
}
static inline SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
    (void)name;
    SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof(*t));
    struct wii_thread_args *a = (struct wii_thread_args *)malloc(sizeof(*a));
    if (!t || !a) { free(t); free(a); return NULL; }
    a->fn = fn; a->data = data;
    if (LWP_CreateThread(&t->t, wii_thread_tramp, a, NULL, 32 * 1024, 64) != 0) {
        free(t); free(a); return NULL;
    }
    return t;
}
static inline void SDL_WaitThread(SDL_Thread *t, int *status)
{
    void *ret = NULL;
    if (t) { LWP_JoinThread(t->t, &ret); free(t); }
    if (status) *status = (int)(intptr_t)ret;
}
static inline int SDL_SetThreadPriority(int p) { (void)p; return 0; }

static inline void SDL_AtomicSet(SDL_atomic_t *a, int v) { __sync_lock_test_and_set(&a->value, v); }
static inline int SDL_AtomicGet(SDL_atomic_t *a) { return __sync_fetch_and_add(&a->value, 0); }
static inline int SDL_AtomicAdd(SDL_atomic_t *a, int v) { return __sync_fetch_and_add(&a->value, v); }

static inline uint64_t SDL_GetPerformanceCounter(void) { return gettime(); }
static inline uint64_t SDL_GetPerformanceFrequency(void) { return TB_TIMER_CLOCK * 1000ULL; }
static inline uint32_t SDL_GetTicks(void) { return (uint32_t)ticks_to_millisecs(gettime()); }
static inline uint64_t SDL_GetTicks64(void) { return ticks_to_millisecs(gettime()); }
extern void udelay(unsigned int us);
static inline void SDL_Delay(uint32_t ms) { udelay(ms * 1000u); }

#endif
