/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"
#include "reimpl/bits/_errno_bionic.h"
#include "ssx/perf.h"


#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->stack_size = 0; // requested stack size, see pthread_create_soloader
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}


/*
 * SSX port: Bionic's pthread_mutex_t / pthread_cond_t are one 32-bit word. We
 * store a pointer to our own mutex / condvar object in it. Statically
 * initialized objects hold one of Bionic's initializer values instead (0,
 * 0x4000, 0x8000), which no heap pointer can equal, so the word itself says
 * whether the real object exists yet.
 *
 * The objects are not newlib's: pthread-embedded backs every mutex with a
 * kernel semaphore and every condvar with three. On Android a mutex or condvar
 * holds no resource, so the game often frees them without calling destroy;
 * here each one leaked kernel objects until creating new ones failed (in the
 * first race, after a few minutes), and the game then used condvars whose
 * init had failed and crashed.
 *
 * So mutexes and condvars here are plain words in the heap, changed with
 * atomics. A thread that has to sleep queues itself in one of a few shared
 * "parking lot" buckets (picked by the address it waits on) and sleeps on a
 * semaphore from a pool; whoever changes that address wakes the threads queued
 * on it. Uncontended lock/unlock make no system call at all. (A first version
 * slept on lightweight condvars, and froze the game at random after a few
 * minutes.)
 *
 * The original code kept a 1024-entry table of initialized objects instead:
 * every lock scanned it under a global lock, two threads could both create
 * the real object for the same mutex, and once the game had more than 1024
 * mutexes and condvars, new ones were re-created on every lock (no mutual
 * exclusion at all). EA's job system then live-locked at the first loading
 * screen.
 */

// These functions return Bionic error numbers (the game compares them with
// ETIMEDOUT = 110 and so on, which newlib numbers differently); the sem_*
// ones set newlib errno, which __errno_soloader translates.

typedef struct ssx_mutex {
    volatile uint32_t state; // 0 unlocked, 1 locked, 2 locked and may have waiters
    volatile SceUID owner;   // recursive / errorcheck mutexes only
    uint32_t count;          // recursion depth, same
    int kind;                // PTHREAD_MUTEX_NORMAL / _RECURSIVE / _ERRORCHECK
} ssx_mutex;

typedef struct ssx_cond {
    volatile uint32_t seq;     // bumped by every signal / broadcast
    volatile uint32_t waiters; // threads in pthread_cond_(timed)wait
} ssx_cond;

#define PARK_BUCKETS 64
#define MUTEX_SPINS  100
#define SEMA_POOL    256

// A thread sleeping in park(), on its own stack
typedef struct park_waiter {
    const void *addr;
    SceUID sema;
    struct park_waiter *next;
} park_waiter;

typedef struct {
    SceKernelLwMutexWork lock;
    park_waiter *head;
} park_bucket;

static park_bucket park_buckets[PARK_BUCKETS];
static SceKernelLwMutexWork pthr_init_lock;

// Semaphores for sleeping threads, reused (count 0 when in the pool)
static SceKernelLwMutexWork sema_pool_lock;
static SceUID sema_pool[SEMA_POOL];
static int sema_pool_count;

__attribute__((constructor)) static void pthr_sync_init(void) {
    sceKernelCreateLwMutex(&pthr_init_lock, "pthr_init_lock", 0, 0, NULL);
    sceKernelCreateLwMutex(&sema_pool_lock, "pthr_sema_pool", 0, 0, NULL);
    for (int i = 0; i < PARK_BUCKETS; i++)
        sceKernelCreateLwMutex(&park_buckets[i].lock, "pthr_park", 0, 0, NULL);
}

static SceUID sema_get(void) {
    SceUID sema = -1;
    sceKernelLockLwMutex(&sema_pool_lock, 1, NULL);
    if (sema_pool_count)
        sema = sema_pool[--sema_pool_count];
    sceKernelUnlockLwMutex(&sema_pool_lock, 1);
    if (sema < 0)
        sema = sceKernelCreateSema("pthr_park", 0, 0, 1, NULL);
    return sema;
}

static void sema_put(SceUID sema) {
    sceKernelLockLwMutex(&sema_pool_lock, 1, NULL);
    if (sema_pool_count < SEMA_POOL) {
        sema_pool[sema_pool_count++] = sema;
        sema = -1;
    }
    sceKernelUnlockLwMutex(&sema_pool_lock, 1);
    if (sema >= 0)
        sceKernelDeleteSema(sema);
}

PTHR_INLINE park_bucket *park_for(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    return &park_buckets[((a >> 4) ^ (a >> 12)) % PARK_BUCKETS];
}

// Sleeps until unpark_all(addr), if *word still equals expected (checked
// under the bucket lock, which unpark_all takes too, so no wakeup is lost).
// Returns 1 on timeout, 0 otherwise (woken, or *word had changed).
static int park(const void *addr, volatile uint32_t *word, uint32_t expected, SceUInt32 timeout_us, int timed) {
    park_bucket *b = park_for(addr);
    park_waiter w = { addr, -1, NULL };

    sceKernelLockLwMutex(&b->lock, 1, NULL);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) {
        sceKernelUnlockLwMutex(&b->lock, 1);
        return 0;
    }
    w.sema = sema_get();
    if (w.sema < 0) {
        // Out of kernel objects: don't spin, back off and let the caller retry
        sceKernelUnlockLwMutex(&b->lock, 1);
        sceKernelDelayThread(1000);
        return 0;
    }
    w.next = b->head;
    b->head = &w;
    sceKernelUnlockLwMutex(&b->lock, 1);

    SceUInt32 t = timeout_us;
    int timed_out = 0;
    if (sceKernelWaitSema(w.sema, 1, timed ? &t : NULL) < 0) {
        // Timed out: take ourselves off the queue, unless unpark_all already
        // did, in which case its signal is coming and has to be consumed so
        // that the semaphore goes back to the pool at 0
        sceKernelLockLwMutex(&b->lock, 1, NULL);
        int queued = 0;
        for (park_waiter **pp = &b->head; *pp; pp = &(*pp)->next) {
            if (*pp == &w) {
                *pp = w.next;
                queued = 1;
                break;
            }
        }
        sceKernelUnlockLwMutex(&b->lock, 1);
        if (queued)
            timed_out = 1;
        else
            sceKernelWaitSema(w.sema, 1, NULL);
    }
    sema_put(w.sema);
    return timed_out;
}

// Wakes every thread parked on addr
static void unpark_all(const void *addr) {
    park_bucket *b = park_for(addr);
    sceKernelLockLwMutex(&b->lock, 1, NULL);
    park_waiter **pp = &b->head;
    while (*pp) {
        park_waiter *w = *pp;
        if (w->addr == addr) {
            *pp = w->next; // unlinked before the signal: w is gone once it wakes
            sceKernelSignalSema(w->sema, 1);
        } else {
            pp = &w->next;
        }
    }
    sceKernelUnlockLwMutex(&b->lock, 1);
}

static int ssx_mutex_lock(ssx_mutex *m, int try) {
    SceUID self = 0;
    if (m->kind != PTHREAD_MUTEX_NORMAL) {
        self = sceKernelGetThreadId();
        if (m->owner == self) {
            if (m->kind == PTHREAD_MUTEX_RECURSIVE) {
                m->count++;
                return 0;
            }
            return try ? EBUSY_BIONIC : EDEADLK_BIONIC;
        }
    }

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if (try)
            return EBUSY_BIONIC;
        // Locks are usually held briefly: spin a little before sleeping
        for (int i = 0; i < MUTEX_SPINS; i++) {
            expected = 0;
            if (__atomic_load_n(&m->state, __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                goto locked;
        }
        // Mark the mutex contended, so that its unlock wakes us
        uint64_t t_wait = sceKernelGetProcessTimeWide();
        while (__atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE) != 0)
            park(m, &m->state, 2, 0, 0);
        perf_wait_add(PERF_WAIT_MUTEX, (uint32_t)(sceKernelGetProcessTimeWide() - t_wait));
    }
locked:
    if (self) {
        m->owner = self;
        m->count = 1;
    }
    return 0;
}

static int ssx_mutex_unlock(ssx_mutex *m) {
    if (m->kind != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != sceKernelGetThreadId())
            return EPERM_BIONIC;
        if (--m->count)
            return 0;
        m->owner = 0;
    }
    if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
        unpark_all(m);
    return 0;
}

// Microseconds until abstime (CLOCK_REALTIME, as Bionic's condvars use)
static int64_t us_until(const struct timespec *abstime) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return ((int64_t)abstime->tv_sec - now.tv_sec) * 1000000 +
           (abstime->tv_nsec / 1000 - now.tv_usec);
}

static int ssx_cond_wait(ssx_cond *c, ssx_mutex *m, const struct timespec *abstime) {
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);

    // Release the mutex completely, even a recursive one held several times
    uint32_t depth = m->count;
    if (m->kind == PTHREAD_MUTEX_RECURSIVE)
        m->count = 1;
    int ret = ssx_mutex_unlock(m);
    if (ret != 0) {
        m->count = depth;
        return ret;
    }

    // A broadcast bumps seq before it reads waiters, and we count ourselves
    // before park() reads seq, so one of the two always sees the other
    __atomic_add_fetch(&c->waiters, 1, __ATOMIC_SEQ_CST);
    uint64_t t_wait = sceKernelGetProcessTimeWide();
    while (__atomic_load_n(&c->seq, __ATOMIC_SEQ_CST) == seq) {
        if (abstime) {
            int64_t left = us_until(abstime);
            if (left <= 0) {
                ret = ETIMEDOUT_BIONIC;
                break;
            }
            park(c, &c->seq, seq, left > 0x7FFFFFFF ? 0x7FFFFFFF : (SceUInt32)left, 1);
        } else {
            park(c, &c->seq, seq, 0, 0);
        }
    }
    __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_SEQ_CST);
    perf_wait_add(PERF_WAIT_COND, (uint32_t)(sceKernelGetProcessTimeWide() - t_wait));

    ssx_mutex_lock(m, 0);
    if (m->kind == PTHREAD_MUTEX_RECURSIVE)
        m->count = depth;
    return ret;
}

// Signal wakes every waiter too, which POSIX allows (spurious wakeups)
static void ssx_cond_broadcast(ssx_cond *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST))
        unpark_all(c);
}

PTHR_INLINE int is_bionic_static_mutex(uintptr_t v) {
    return v == BIONIC_PTHREAD_MUTEX_INITIALIZER ||
           v == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER ||
           v == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
}

// Creates the real mutex and publishes it in the Bionic word
static int _mutex_t_create(pthread_mutex_t_bionic *mutex, int kind) {
    ssx_mutex *real = calloc(1, sizeof(ssx_mutex));
    if (!real) {
        l_error("mutex initialization for %p has failed", mutex);
        return ENOMEM_BIONIC;
    }
    real->kind = kind;
    __atomic_store_n(&mutex->real_ptr, real, __ATOMIC_RELEASE);
    return 0;
}

// pthread_mutex_init: always creates (the struct may hold garbage)
static int _mutex_t_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
    int kind = PTHREAD_MUTEX_NORMAL;
    if (attr)
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    if (kind != PTHREAD_MUTEX_RECURSIVE && kind != PTHREAD_MUTEX_ERRORCHECK)
        kind = PTHREAD_MUTEX_NORMAL;
    return _mutex_t_create(mutex, kind);
}

// Lock/unlock/...: the real mutex, created on first use of a static one
// null check for `mutex` param must be performed before this
static ssx_mutex *_mutex_t_get(pthread_mutex_t_bionic *mutex) {
    uintptr_t v = (uintptr_t)__atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE);
    if (!is_bionic_static_mutex(v))
        return (ssx_mutex *)v;

    sceKernelLockLwMutex(&pthr_init_lock, 1, NULL);
    v = (uintptr_t)__atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE);
    if (is_bionic_static_mutex(v)) {
        int kind = PTHREAD_MUTEX_NORMAL;
        if (v == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (v == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
        _mutex_t_create(mutex, kind);
    }
    sceKernelUnlockLwMutex(&pthr_init_lock, 1);
    v = (uintptr_t)__atomic_load_n(&mutex->real_ptr, __ATOMIC_ACQUIRE);
    return is_bionic_static_mutex(v) ? NULL : (ssx_mutex *)v;
}

// Creates the real condvar and publishes it in the Bionic word
static int _cond_t_create(pthread_cond_t_bionic *cond) {
    ssx_cond *real = calloc(1, sizeof(ssx_cond));
    if (!real) {
        l_error("cond initialization for %p has failed", cond);
        return ENOMEM_BIONIC;
    }
    __atomic_store_n(&cond->real_ptr, real, __ATOMIC_RELEASE);
    return 0;
}

// Wait/signal/...: the real condvar, created on first use of a static one
// null check for `cond` param must be performed before this
static ssx_cond *_cond_t_get(pthread_cond_t_bionic *cond) {
    ssx_cond *v = __atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE);
    if (v != (ssx_cond *)BIONIC_PTHREAD_COND_INITIALIZER)
        return v;

    sceKernelLockLwMutex(&pthr_init_lock, 1, NULL);
    if (__atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE) == (ssx_cond *)BIONIC_PTHREAD_COND_INITIALIZER)
        _cond_t_create(cond);
    sceKernelUnlockLwMutex(&pthr_init_lock, 1);
    return __atomic_load_n(&cond->real_ptr, __ATOMIC_ACQUIRE);
}

// Runs a game thread, registered with the profiler first
typedef struct {
    void *(*start)(void *);
    void *param;
} thread_start;

static void *thread_trampoline(void *arg) {
    thread_start s = *(thread_start *)arg;
    free(arg);
    perf_thread_register((uintptr_t)pthread_self(), "thread");
    return s.start(s.param);
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    thread_start *ts = malloc(sizeof(*ts));
    if (ts) {
        ts->start = start;
        ts->param = param;
        start = thread_trampoline;
        param = ts;
    }

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        // Thread stacks come out of main RAM, which is scarce: honour the size
        // the game asked for (with some margin for our wrappers), and use the
        // large default only when it did not ask (EA's audio threads ask for
        // 32 KB; forcing 512 KB on them failed with EAGAIN).
        // attr->stack_size holds what pthread_attr_setstacksize asked for
        size_t stacksize = attr->stack_size;
        if (stacksize == 0)
            stacksize = 512 * 1024;
        else if (stacksize < 128 * 1024)
            stacksize = 128 * 1024;
        pthread_attr_setstacksize(attr->real_ptr, stacksize);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    if (ret != 0 && ts && start == thread_trampoline)
        free(ts);
    if (ret != 0)
        l_error("pthread_create failed: %d", ret);

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    if (is_bionic_static_mutex((uintptr_t)mutex->real_ptr)) return 0; // never used
    free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return 0;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    ssx_mutex *m = _mutex_t_get(mutex);
    if (!m) return ENOMEM_BIONIC;
    return ssx_mutex_lock(m, 0);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    ssx_mutex *m = _mutex_t_get(mutex);
    if (!m) return ENOMEM_BIONIC;
    return ssx_mutex_lock(m, 1);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    ssx_mutex *m = mutex->real_ptr;
    if (is_bionic_static_mutex((uintptr_t)m)) return EPERM_BIONIC; // never locked
    return ssx_mutex_unlock(m);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

// pthread_cond_init: always creates (the struct may hold garbage)
int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond) return EINVAL;
    return _cond_t_create(cond);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    if (cond->real_ptr == (void *)BIONIC_PTHREAD_COND_INITIALIZER) return 0; // never used
    free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return 0;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    ssx_cond *c = _cond_t_get(cond);
    if (!c) return ENOMEM_BIONIC;
    ssx_cond_broadcast(c);
    return 0;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    ssx_cond *c = _cond_t_get(cond);
    if (!c) return ENOMEM_BIONIC;
    ssx_cond_broadcast(c);
    return 0;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex || !abstime) return EINVAL;
    ssx_cond *c = _cond_t_get(cond);
    ssx_mutex *m = _mutex_t_get(mutex);
    if (!c || !m) return ENOMEM_BIONIC;
    return ssx_cond_wait(c, m, abstime);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;
    ssx_cond *c = _cond_t_get(cond);
    ssx_mutex *m = _mutex_t_get(mutex);
    if (!c || !m) return ENOMEM_BIONIC;
    return ssx_cond_wait(c, m, NULL);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    // Always start fresh: an attr on the stack can hold a stale magic value
    // from an earlier one at the same address, and the old code would then
    // reuse a destroyed attr.
    attr->magic = 0;
    attr->stack_size = 0;
    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    attr->stack_size = stacksize; // see pthread_create_soloader
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
    int r = pthread_setschedparam(thread, policy, param);
    l_info("[THREAD] pthread_setschedparam(0x%x, policy %d, priority %d) = %d (Vita priority now 0x%x)",
           (unsigned)thread, policy, param ? param->sched_priority : -1, r,
           (unsigned)sceKernelGetThreadCurrentPriority());
    return r;
}

// EAThread names its threads with prctl(PR_SET_NAME)
int prctl_soloader(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5) {
    l_debug("[THREAD] prctl(%d, %s) on thread 0x%x", option, option == 15 && arg2 ? (const char *)arg2 : "-",
            (unsigned)sceKernelGetThreadId());
    if (option == 15 && arg2) { // PR_SET_NAME
        perf_thread_set_name((uintptr_t)pthread_self(), (const char *)arg2);
        // The audio mixer ("EAAudioCore Dac") and its submit thread need little
        // CPU but must never wait behind the job threads, or the sound drops out
        if (strncmp((const char *)arg2, "EAAudioCore", 11) == 0)
            sceKernelChangeThreadPriority(0, 0x80);
        // The render thread is the frame's critical path (~90% of a core in a
        // race) while 4 job workers keep the other cores busy at the same
        // priority: let it run first, the workers take the rest
        else if (strcmp((const char *)arg2, "FGGameRender") == 0)
            sceKernelChangeThreadPriority(0, 0x98);
        l_info("[THREAD] \"%s\" is thread 0x%x (Vita priority 0x%x)", (const char *)arg2,
               (unsigned)sceKernelGetThreadId(), (unsigned)sceKernelGetThreadCurrentPriority());
    }
    return 0;
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    perf_thread_set_name((uintptr_t)thread, thread_name); // any length
    l_info("[THREAD] pthread_setname_np(0x%x, \"%s\")", (unsigned)thread, thread_name);
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    if (!abstime) return -1;
    int64_t left = us_until(abstime);
    SceUInt32 timeout = left <= 0 ? 0 : left > 0x7FFFFFFF ? 0x7FFFFFFF : (SceUInt32)left;
    uint64_t t_wait = sceKernelGetProcessTimeWide();
    int r = sceKernelWaitSema(*uid, 1, &timeout);
    perf_wait_add(PERF_WAIT_SEM, (uint32_t)(sceKernelGetProcessTimeWide() - t_wait));
    if (r < 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

int sem_trywait_soloader (int * uid) {
    if (sceKernelPollSema(*uid, 1) < 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int sem_wait_soloader (int * uid) {
    uint64_t t_wait = sceKernelGetProcessTimeWide();
    int r = sceKernelWaitSema(*uid, 1, NULL);
    perf_wait_add(PERF_WAIT_SEM, (uint32_t)(sceKernelGetProcessTimeWide() - t_wait));
    if (r < 0)
        return -1;
    return 0;
}
