/*
 * hakuX HLE GPU — threads, events and timers over pthreads (thread.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* sched_setaffinity */
#endif
#include "hw/xbox/hle/util/thread.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __ANDROID__
#include "hw/xbox/hle/prop.h"
#endif

static void monotonic_cond_init(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

void hle_cond_init(HleCond *c)
{
    monotonic_cond_init(&c->c);
}

void hle_cond_destroy(HleCond *c)
{
    pthread_cond_destroy(&c->c);
}

bool hle_cond_timedwait(HleCond *c, HleMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}

void hle_sem_init(HleSem *s, int init)
{
    sem_init(&s->s, 0, (unsigned)init);
}

void hle_sem_destroy(HleSem *s)
{
    sem_destroy(&s->s);
}

void hle_sem_post(HleSem *s)
{
    sem_post(&s->s);
}

void hle_sem_wait(HleSem *s)
{
    while (sem_wait(&s->s) != 0 && errno == EINTR) {
    }
}

bool hle_sem_trywait(HleSem *s)
{
    return sem_trywait(&s->s) == 0;
}

bool hle_sem_timedwait(HleSem *s, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    int r;
    while ((r = sem_timedwait(&s->s, &ts)) != 0 && errno == EINTR) {
    }
    return r == 0;
}

void hle_event_init(HleEvent *e, bool set)
{
    pthread_mutex_init(&e->m, NULL);
    monotonic_cond_init(&e->c);
    e->set = set;
}

void hle_event_destroy(HleEvent *e)
{
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
}

void hle_event_set(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}

void hle_event_reset(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}

void hle_event_wait(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    while (!e->set) {
        pthread_cond_wait(&e->c, &e->m);
    }
    pthread_mutex_unlock(&e->m);
}

void hle_thread_create(HleThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&t->t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (name) {
        char n[16];
        snprintf(n, sizeof(n), "%s", name);
        pthread_setname_np(t->t, n);
    }
    if (mode == HLE_THREAD_DETACHED) {
        pthread_detach(t->t);
    }
}

void *hle_thread_join(HleThread *t)
{
    void *r = NULL;
    pthread_join(t->t, &r);
    return r;
}

/* ---- timers ---------------------------------------------------------------- */

struct HleTimer {
    void (*cb)(void *);
    void *opaque;
    int64_t deadline_ms;        /* -1: not armed */
    HleTimer *next;
};

static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tc;
static HleTimer *g_timers;
static pthread_once_t g_tonce = PTHREAD_ONCE_INIT;

static void *timer_thread(void *arg)
{
    pthread_mutex_lock(&g_tm);
    for (;;) {
        int64_t now = hle_now_ms(), next = INT64_MAX;
        HleTimer *due = NULL;
        for (HleTimer *t = g_timers; t; t = t->next) {
            if (t->deadline_ms >= 0 && t->deadline_ms <= now) {
                due = t;
                break;
            }
            if (t->deadline_ms >= 0 && t->deadline_ms < next) {
                next = t->deadline_ms;
            }
        }
        if (due) {
            due->deadline_ms = -1;
            pthread_mutex_unlock(&g_tm);
            due->cb(due->opaque);
            pthread_mutex_lock(&g_tm);
            continue;
        }
        if (next == INT64_MAX) {
            pthread_cond_wait(&g_tc, &g_tm);
        } else {
            struct timespec ts = { (time_t)(next / 1000), (long)(next % 1000) * 1000000 };
            pthread_cond_timedwait(&g_tc, &g_tm, &ts);
        }
    }
    return arg;
}

static void timers_start(void)
{
    monotonic_cond_init(&g_tc);
    pthread_t t;
    pthread_create(&t, NULL, timer_thread, NULL);
    pthread_setname_np(t, "hle-timers");
    pthread_detach(t);
}

HleTimer *hle_timer_new(void (*cb)(void *), void *opaque)
{
    pthread_once(&g_tonce, timers_start);
    HleTimer *t = calloc(1, sizeof(*t));
    t->cb = cb;
    t->opaque = opaque;
    t->deadline_ms = -1;
    pthread_mutex_lock(&g_tm);
    t->next = g_timers;
    g_timers = t;
    pthread_mutex_unlock(&g_tm);
    return t;
}

void hle_timer_mod(HleTimer *t, int64_t deadline_ms)
{
    pthread_mutex_lock(&g_tm);
    t->deadline_ms = deadline_ms;
    pthread_cond_broadcast(&g_tc);
    pthread_mutex_unlock(&g_tm);
}

void hle_log(const char *tag, const char *fmt, ...);

#ifdef __linux__
static struct {
    int tid[HLE_PIN_N][8];          /* the role's threads (audio: several) */
    int ntid[HLE_PIN_N];
    int mode;                       /* applied; -1 none yet */
    bool probed, mixed;
    cpu_set_t fast, slow, all;
    int nfast, ncpu;
} g_pin = { .mode = -1 };

/* The fastest cores (the highest cpu_capacity) and the others. */
static void pin_probe(void)
{
    g_pin.probed = true;
    int cap[64], best = 0, n = 0;
    for (int c = 0; c < 64; c++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        FILE *f = fopen(path, "r");
        if (!f) {
            break;
        }
        if (fscanf(f, "%d", &cap[c]) != 1) {
            cap[c] = 0;
        }
        fclose(f);
        best = cap[c] > best ? cap[c] : best;
        n = c + 1;
    }
    CPU_ZERO(&g_pin.fast);
    CPU_ZERO(&g_pin.slow);
    CPU_ZERO(&g_pin.all);
    for (int c = 0; c < n; c++) {
        CPU_SET(c, &g_pin.all);
        if (cap[c] == best) {
            CPU_SET(c, &g_pin.fast);
            g_pin.nfast++;
        } else {
            CPU_SET(c, &g_pin.slow);
        }
    }
    g_pin.ncpu = n;
    g_pin.mixed = best && g_pin.nfast < n;
}

/* The game's setting (HAKUX_HLE_PIN, the app's threads=), else 1; debug:
 * debug.hakux.pin over it, live */
static int pin_mode(void)
{
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX];
    if (hle_prop_get("debug.hakux.pin", v) && v[0] >= '0' && v[0] <= '3' && !v[1]) {
        return v[0] - '0';
    }
#endif
    const char *e = getenv("HAKUX_HLE_PIN");
    return e && e[0] >= '0' && e[0] <= '3' && !e[1] ? e[0] - '0' : 1;
}

/* Role r's cores in mode m. */
static const cpu_set_t *pin_set(int m, int r)
{
    /* A anywhere, F the fastest cores, S the others. The vCPU kept off the
     * fastest cores (an earlier mode 1) made Sega GT 2002 guest-bound: 33
     * fps against 43 with it anywhere. */
    enum { A, F, S };
    static const uint8_t where[4][HLE_PIN_N] = {
        /* vcpu render decode audio */
        { A, A, A, A },
        { A, F, F, S },
        { F, F, S, S },
        { F, S, F, S },
    };
    const uint8_t w = where[m][r];
    return w == F ? &g_pin.fast : w == S ? &g_pin.slow : &g_pin.all;
}

static void pin_apply(int m)
{
    for (int r = 0; r < HLE_PIN_N; r++) {
        for (int i = 0; i < __atomic_load_n(&g_pin.ntid[r], __ATOMIC_ACQUIRE); i++) {
            sched_setaffinity(g_pin.tid[r][i], sizeof(cpu_set_t), pin_set(m, r));
        }
    }
}
#endif

void hle_thread_role_tid(int role, int tid)
{
#ifdef __linux__
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, pin_probe);
    const int i = __atomic_load_n(&g_pin.ntid[role], __ATOMIC_RELAXED);
    if (i < 8) {
        g_pin.tid[role][i] = tid;
        __atomic_store_n(&g_pin.ntid[role], i + 1, __ATOMIC_RELEASE);
    }
    if (g_pin.mixed) {
        sched_setaffinity(tid, sizeof(cpu_set_t), pin_set(pin_mode(), role));
    }
#else
    (void)role;
    (void)tid;
#endif
}

void hle_thread_role(int role)
{
#ifdef __linux__
    hle_thread_role_tid(role, (int)gettid());
#else
    (void)role;
#endif
}

void hle_thread_pin_poll(void)
{
#ifdef __linux__
    if (!g_pin.probed || !g_pin.mixed) {
        return;
    }
    int m = pin_mode();
    if (m != g_pin.mode) {
        g_pin.mode = m;
        pin_apply(m);
        static const char *const names[4] = {
            "anywhere", "render and decoder on the fastest cores, the vCPU anywhere",
            "vCPU and render on the fastest cores", "vCPU and decoder on the fastest cores" };
        /* modes 1-3 also keep the audio threads on the other cores */
        hle_log("hakuX-hle", "threads: %s (%d of %d cores fast; debug.hakux.pin=%d)", names[m],
                g_pin.nfast, g_pin.ncpu, m);
    }
#endif
}
