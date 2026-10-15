// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: kernel event flags (runtime_eventflag.c), resolved by name through NID#libkernel.
 * Contracts from the PS4 SDK as implemented by shadPS4 b4e7ae7
 * src/core/libraries/kernel/threads/event_flag.cpp (reference only, not copied). */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

typedef int32_t(ABI* Create)(uint64_t*, const char*, uint32_t, uint64_t, const void*);
typedef int32_t(ABI* Handle)(uint64_t);
typedef int32_t(ABI* Bits)(uint64_t, uint64_t);
typedef int32_t(ABI* Poll)(uint64_t, uint64_t, uint32_t, uint64_t*);
typedef int32_t(ABI* Wait)(uint64_t, uint64_t, uint32_t, uint64_t*, uint32_t*);

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
enum {
    EPERM_ = 1,
    ESRCH_ = 3,
    EACCES_ = 13,
    EBUSY_ = 16,
    EINVAL_ = 22,
    ETIMEDOUT_ = 60,
    ENAMETOOLONG_ = 63
};
enum { AND = 1, OR = 2, CLEAR_ALL = 0x10, CLEAR_PAT = 0x20 };
enum { SINGLE = 0x10, MULTI = 0x20, FIFO = 1 };

static Create create;
static Handle destroy;
static Bits set, clear;
static Poll poll;
static Wait wait;

static void* resolve(const char* name) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#libkernel", nid);
    void* f = (void*)runtime_resolve(key, 0);
    assert(f);
    return f;
}

static void arguments(void) {
    uint64_t ef = 0;
    assert(create(NULL, "x", 0, 0, NULL) == ERR(EINVAL_));
    assert(create(&ef, NULL, 0, 0, NULL) == ERR(EINVAL_));
    assert(create(&ef, "x", 0x40, 0, NULL) == ERR(EINVAL_));
    assert(create(&ef, "0123456789012345678901234567890123", 0, 0, NULL) == ERR(ENAMETOOLONG_));
    assert(destroy(0) == ERR(ESRCH_) && destroy(12345) == ERR(ESRCH_));
    assert(set(777, 1) == ERR(ESRCH_) && clear(777, 1) == ERR(ESRCH_));
    assert(create(&ef, "args", MULTI | FIFO, 0, NULL) == 0 && ef);
    uint64_t r = 0;
    assert(poll(ef, 0, AND, &r) == ERR(EINVAL_));
    assert(poll(ef, 1, 3, &r) == ERR(EINVAL_)); /* neither AND nor OR */
    assert(destroy(ef) == 0 && destroy(ef) == ERR(ESRCH_));
}

static void patterns(void) {
    uint64_t ef = 0, r = 0;
    assert(create(&ef, "pat", MULTI, 0x5, NULL) == 0);
    assert(poll(ef, 0x3, AND, &r) == ERR(EBUSY_) && r == 0x5);
    assert(poll(ef, 0x3, OR, &r) == 0 && r == 0x5);
    assert(set(ef, 0x2) == 0);
    assert(poll(ef, 0x3, AND | CLEAR_PAT, &r) == 0 && r == 0x7);
    assert(poll(ef, 0x4, AND, &r) == 0 && r == 0x4); /* only 0x3 was cleared */
    assert(clear(ef, 0x0) == 0);                     /* clear keeps the given bits */
    assert(poll(ef, 0x4, OR, &r) == ERR(EBUSY_) && r == 0);
    assert(set(ef, 0xf0) == 0 && poll(ef, 0x10, OR | CLEAR_ALL, &r) == 0 && r == 0xf0);
    assert(poll(ef, 0xf0, OR, &r) == ERR(EBUSY_));
    assert(destroy(ef) == 0);
}

static void timeout(void) {
    uint64_t ef = 0, r = 9;
    uint32_t micros = 20000;
    assert(create(&ef, "time", MULTI, 0, NULL) == 0);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    assert(wait(ef, 1, AND, &r, &micros) == ERR(ETIMEDOUT_) && micros == 0 && r == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    assert((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) >= 15000000L);
    micros = 1000000;
    assert(set(ef, 1) == 0 && wait(ef, 1, AND, &r, &micros) == 0 && r == 1 && micros > 0);
    assert(destroy(ef) == 0);
}

struct waiter {
    uint64_t ef, bits, result;
    uint32_t mode;
    int32_t status;
};
static void* wait_thread(void* arg) {
    struct waiter* w = arg;
    w->status = wait(w->ef, w->bits, w->mode, &w->result, NULL);
    return NULL;
}
static void sleep_ms(long ms) {
    struct timespec t = {0, ms * 1000000L};
    nanosleep(&t, NULL);
}

static void wakeups(void) {
    uint64_t ef = 0;
    assert(create(&ef, "wake", SINGLE, 0, NULL) == 0);
    struct waiter w = {ef, 0x6, 0, AND | CLEAR_PAT, -1};
    pthread_t t;
    assert(!pthread_create(&t, NULL, wait_thread, &w));
    sleep_ms(20);
    uint64_t r = 0;
    assert(wait(ef, 1, OR, &r, NULL) == ERR(EPERM_)); /* single-waiter flag */
    assert(set(ef, 0x2) == 0);
    sleep_ms(10);
    assert(w.status == -1); /* 0x4 still missing */
    assert(set(ef, 0x4 | 0x8) == 0);
    assert(!pthread_join(t, NULL));
    assert(w.status == 0 && w.result == 0xe);
    assert(poll(ef, 0x8, AND, &r) == 0 && r == 0x8); /* only the waited bits were cleared */
    struct waiter d = {ef, 0x100, 0, AND, -1};
    assert(!pthread_create(&t, NULL, wait_thread, &d));
    sleep_ms(20);
    assert(destroy(ef) == 0); /* wakes the waiter */
    assert(!pthread_join(t, NULL));
    assert(d.status == ERR(EACCES_));
}

int main(void) {
    runtime_start(1);
    create = (Create)resolve("sceKernelCreateEventFlag");
    destroy = (Handle)resolve("sceKernelDeleteEventFlag");
    set = (Bits)resolve("sceKernelSetEventFlag");
    clear = (Bits)resolve("sceKernelClearEventFlag");
    poll = (Poll)resolve("sceKernelPollEventFlag");
    wait = (Wait)resolve("sceKernelWaitEventFlag");
    arguments();
    patterns();
    timeout();
    wakeups();
    puts("PASS: event flags (create, patterns, clear modes, timeout, single waiter, delete wakes)");
    return 0;
}
