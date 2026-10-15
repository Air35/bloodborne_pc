// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: kernel event flags (libkernel sceKernel*EventFlag), a system library any title can
 * import (30 static call sites in the first target, docs/plans/2026-10-14-a2-inventario-imports.md).
 * Contracts: PS4 SDK
 * behaviour as implemented by shadPS4 b4e7ae7 src/core/libraries/kernel/threads/event_flag.cpp
 * (consulted, reimplemented): AND/OR waits, clear none/all/waited bits, Poll = EBUSY instead of
 * ETIMEDOUT, single-waiter flags refuse a second waiter (EPERM), Delete wakes waiters (EACCES).
 * Handles are validated against a registry, so a stale or forged handle returns ESRCH.
 * Thread safety: one module mutex; waiters sleep on their own condition variable. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime.h"

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n))) /* FreeBSD errno, as runtime_sema.c */
enum {
    E_PERM = 1,
    E_SRCH = 3,
    E_NOMEM = 12,
    E_ACCES = 13,
    E_BUSY = 16,
    E_INVAL = 22,
    E_TIMEDOUT = 60,
    E_NAMETOOLONG = 63
};
enum { WAIT_AND = 0x01, WAIT_OR = 0x02, CLEAR_ALL = 0x10, CLEAR_PAT = 0x20 };
enum { ATTR_QUEUE_MASK = 0x0f, ATTR_THREAD_MASK = 0xf0, ATTR_MULTI = 0x20 };
enum { WAITING, READY, DELETED };

typedef struct Waiter {
    uint64_t bits, result;
    uint32_t mode;
    int state;
    pthread_cond_t wake;
    struct Waiter* next;
} Waiter;

typedef struct EventFlag {
    uint64_t bits;
    int multi, deleted;
    unsigned waiters;
    Waiter* head;
    pthread_cond_t gone; /* Delete waits here until every woken waiter has left */
    struct EventFlag* next;
} EventFlag;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static EventFlag* registry;

/* Registry lookup by handle; caller holds the lock. NULL for unknown or deleted handles. */
static EventFlag* find(uint64_t handle) {
    for (EventFlag* e = registry; e; e = e->next)
        if ((uint64_t)(uintptr_t)e == handle && !e->deleted)
            return e;
    return NULL;
}

static int met(const EventFlag* e, uint64_t bits, uint32_t mode) {
    return (mode & 0x0f) == WAIT_AND ? (e->bits & bits) == bits : (e->bits & bits) != 0;
}

static void apply_clear(EventFlag* e, uint64_t bits, uint32_t mode) {
    if ((mode & 0xf0) == CLEAR_ALL)
        e->bits = 0;
    else if ((mode & 0xf0) == CLEAR_PAT)
        e->bits &= ~bits;
}

static int valid_mode(uint32_t mode) {
    const uint32_t wait = mode & 0x0f, clear = mode & 0xf0;
    return (wait == WAIT_AND || wait == WAIT_OR) &&
           (clear == 0 || clear == CLEAR_ALL || clear == CLEAR_PAT) && !(mode & ~0xffu);
}

static void unlink_waiter(EventFlag* e, Waiter* w) {
    for (Waiter** p = &e->head; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            break;
        }
    if (--e->waiters == 0 && e->deleted)
        pthread_cond_signal(&e->gone);
}

static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Sleeps until the waiter is woken or the deadline (0 = none) passes; lock held. */
static int sleep_waiter(Waiter* w, uint64_t deadline) {
    while (w->state == WAITING) {
        if (!deadline) {
            pthread_cond_wait(&w->wake, &lock);
            continue;
        }
        struct timespec at = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
        if (pthread_cond_timedwait(&w->wake, &lock, &at) == ETIMEDOUT && w->state == WAITING)
            return 0;
    }
    return 1;
}

/* Blocks the calling thread on the flag; *timeout (microseconds, NULL = forever) is updated
 * with the time left. Returns 0, or ESRCH/EPERM/EACCES/ETIMEDOUT/ENOMEM. Lock held. */
static int32_t block(EventFlag* e, uint64_t bits, uint32_t mode, uint64_t* result,
                     uint32_t* timeout) {
    if (!e->multi && e->waiters)
        return ERR(E_PERM);
    Waiter w = {.bits = bits, .mode = mode, .state = WAITING};
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr))
        return ERR(E_NOMEM);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    const int failed = pthread_cond_init(&w.wake, &attr);
    pthread_condattr_destroy(&attr);
    if (failed)
        return ERR(E_NOMEM);
    w.next = e->head;
    e->head = &w; /* wake order is irrelevant to the contract: Set checks every waiter */
    ++e->waiters;
    const uint64_t start = monotonic_ns();
    const uint64_t deadline = timeout ? start + (uint64_t)*timeout * 1000u : 0;
    const int woken = sleep_waiter(&w, deadline);
    unlink_waiter(e, &w);
    pthread_cond_destroy(&w.wake);
    if (!woken) {
        if (result)
            *result = e->bits;
        *timeout = 0;
        return ERR(E_TIMEDOUT);
    }
    if (w.state == DELETED)
        return ERR(E_ACCES);
    if (result)
        *result = w.result;
    if (timeout) {
        const uint64_t now = monotonic_ns();
        *timeout = now >= deadline ? 0 : (uint32_t)((deadline - now) / 1000u);
    }
    return 0;
}

/* Wait/Poll body. poll != 0 never blocks and returns EBUSY when the pattern is not met. */
static int32_t sotc_evf_wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result,
                             uint32_t* timeout, int poll) {
    if (!bits || !valid_mode(mode))
        return ERR(E_INVAL);
    pthread_mutex_lock(&lock);
    EventFlag* e = find(handle);
    int32_t status = 0;
    if (!e)
        status = ERR(E_SRCH);
    else if (met(e, bits, mode)) {
        if (result)
            *result = e->bits;
        apply_clear(e, bits, mode);
    } else if (poll || (timeout && !*timeout)) {
        if (result)
            *result = e->bits;
        status = poll ? ERR(E_BUSY) : ERR(E_TIMEDOUT);
    } else {
        status = block(e, bits, mode, result, timeout);
    }
    pthread_mutex_unlock(&lock);
    return status;
}

static int32_t sotc_evf_set(uint64_t handle, uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag* e = find(handle);
    if (e) {
        e->bits |= bits;
        for (Waiter* w = e->head; w; w = w->next) {
            if (w->state != WAITING || !met(e, w->bits, w->mode))
                continue;
            w->result = e->bits;
            apply_clear(e, w->bits, w->mode);
            w->state = READY;
            pthread_cond_signal(&w->wake);
        }
    }
    pthread_mutex_unlock(&lock);
    return e ? 0 : ERR(E_SRCH);
}

/* The SDK's Clear keeps only the given bits (bits &= pattern). */
static int32_t sotc_evf_clear(uint64_t handle, uint64_t keep) {
    pthread_mutex_lock(&lock);
    EventFlag* e = find(handle);
    if (e)
        e->bits &= keep;
    pthread_mutex_unlock(&lock);
    return e ? 0 : ERR(E_SRCH);
}

static int32_t sotc_evf_create(uint64_t* out, const char* name, uint32_t attr, uint64_t bits,
                               const void* options) {
    if (!out || !name || options)
        return ERR(E_INVAL);
    const uint32_t queue = attr & ATTR_QUEUE_MASK, thread = attr & ATTR_THREAD_MASK;
    if (attr & ~0xffu || queue > 2 || (thread && thread != 0x10 && thread != ATTR_MULTI))
        return ERR(E_INVAL);
    if (strlen(name) >= 32)
        return ERR(E_NAMETOOLONG);
    EventFlag* e = calloc(1, sizeof(*e));
    if (!e || pthread_cond_init(&e->gone, NULL)) {
        free(e);
        return ERR(E_NOMEM);
    }
    e->bits = bits;
    e->multi = thread == ATTR_MULTI; /* default (0) and 0x10 are single-waiter */
    pthread_mutex_lock(&lock);
    e->next = registry;
    registry = e;
    pthread_mutex_unlock(&lock);
    *out = (uint64_t)(uintptr_t)e;
    return 0;
}

static int32_t sotc_evf_delete(uint64_t handle) {
    pthread_mutex_lock(&lock);
    EventFlag* e = find(handle);
    if (!e) {
        pthread_mutex_unlock(&lock);
        return ERR(E_SRCH);
    }
    e->deleted = 1;
    for (Waiter* w = e->head; w; w = w->next) {
        w->state = DELETED;
        pthread_cond_signal(&w->wake);
    }
    while (e->waiters)
        pthread_cond_wait(&e->gone, &lock);
    for (EventFlag** p = &registry; *p; p = &(*p)->next)
        if (*p == e) {
            *p = e->next;
            break;
        }
    pthread_mutex_unlock(&lock);
    pthread_cond_destroy(&e->gone);
    free(e);
    return 0;
}

/* Guest wrappers: argument translation only. */
static ABI int32_t hle_create(uint64_t* out, const char* name, uint32_t attr, uint64_t bits,
                              const void* options) {
    return sotc_evf_create(out, name, attr, bits, options);
}
static ABI int32_t hle_delete(uint64_t handle) {
    return sotc_evf_delete(handle);
}
static ABI int32_t hle_set(uint64_t handle, uint64_t bits) {
    return sotc_evf_set(handle, bits);
}
static ABI int32_t hle_clear(uint64_t handle, uint64_t keep) {
    return sotc_evf_clear(handle, keep);
}
static ABI int32_t hle_poll(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result) {
    return sotc_evf_wait(handle, bits, mode, result, NULL, 1);
}
static ABI int32_t hle_wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t* result,
                            uint32_t* timeout) {
    return sotc_evf_wait(handle, bits, mode, result, timeout, 0);
}

static const RuntimeExport exports[] = {
    {"sceKernelCreateEventFlag", hle_create}, {"sceKernelDeleteEventFlag", hle_delete},
    {"sceKernelSetEventFlag", hle_set},       {"sceKernelClearEventFlag", hle_clear},
    {"sceKernelPollEventFlag", hle_poll},     {"sceKernelWaitEventFlag", hle_wait},
};

uintptr_t runtime_eventflag_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
