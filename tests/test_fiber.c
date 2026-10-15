// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libSceFiber HLE (runtime_fiber.c). Contracts from the PS4 SDK as implemented by
 * shadPS4 b4e7ae7 src/core/libraries/fiber/fiber.cpp (consulted, reimplemented). */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

#define FIBER_NULL ((int32_t)0x80590001)
#define FIBER_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_RANGE ((int32_t)0x80590003)
#define FIBER_INVALID ((int32_t)0x80590004)
#define FIBER_PERMISSION ((int32_t)0x80590005)
#define FIBER_STATE ((int32_t)0x80590006)
#define BUILD_450 0x04508000u /* the SotC's SDK (PUBTOOLINFO sdk_ver, docs/02) */

typedef void(ABI* Entry)(uint64_t, uint64_t);
typedef int32_t(ABI* Init)(void*, const char*, Entry, uint64_t, void*, uint64_t, const void*,
                           uint32_t);
typedef int32_t(ABI* OptInit)(void*);
typedef int32_t(ABI* Finalize)(void*);
typedef int32_t(ABI* Run)(void*, uint64_t, uint64_t*);
typedef int32_t(ABI* Switch)(void*, uint64_t, uint64_t*);
typedef int32_t(ABI* ReturnToThread)(uint64_t, uint64_t*);
typedef int32_t(ABI* GetInfo)(void*, void*);
typedef int32_t(ABI* Rename)(void*, const char*);
typedef int32_t(ABI* GetSelf)(void**);

static Init init;
static OptInit opt_init;
static Finalize finalize;
static Run run;
static Switch fswitch;
static ReturnToThread to_thread;
static GetInfo get_info;
static Rename rename_fiber;
static GetSelf get_self;

static void* resolve(const char* name) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#libSceFiber", nid);
    void* f = (void*)runtime_resolve(key, 0);
    assert(f);
    return f;
}

typedef struct {
    _Alignas(16) unsigned char bytes[256];
} FiberObject;
static FiberObject fa, fb;
static _Alignas(16) unsigned char stack_a[65536], stack_b[65536];
static uint64_t log_values[16];
static int log_count;
static void note(uint64_t v) {
    log_values[log_count++] = v;
}

static void ABI entry_a(uint64_t arg_init, uint64_t arg_run) {
    note(arg_init);
    note(arg_run);
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    note(mxcsr);
    void* self = NULL;
    assert(get_self(&self) == 0 && self == &fa);
    unsigned char local;
    assert(&local > stack_a && &local < stack_a + sizeof(stack_a)); /* runs on its context */
    uint64_t r = 0, ignored = 0;
    assert(run(&fb, 0, &ignored) == FIBER_PERMISSION); /* Run only from a thread */
    assert(to_thread(100, &r) == 0);                   /* back in Run(): returns 100 */
    note(r);                                           /* resumed by Run(&fa, 7) */
    assert(fswitch(&fb, 55, &r) == 0);
    note(r); /* resumed by Run(&fa, 9) */
    assert(to_thread(300, &r) == 0);
    for (;;)
        assert(to_thread(0, &r) == 0); /* entries never return */
}

static void ABI entry_b(uint64_t arg_init, uint64_t arg_run) {
    note(arg_init);
    note(arg_run);
    uint64_t r = 0;
    assert(to_thread(200, &r) == 0);
    note(r);
    for (;;)
        assert(to_thread(400, &r) == 0);
}

static void arguments(void) {
    _Alignas(8) unsigned char opt[128];
    assert(opt_init(NULL) == FIBER_NULL && opt_init(opt + 4) == FIBER_ALIGNMENT);
    assert(opt_init(opt) == 0);
    assert(init(NULL, "x", entry_a, 0, stack_a, sizeof(stack_a), NULL, BUILD_450) == FIBER_NULL);
    assert(init(&fa, "x", NULL, 0, stack_a, sizeof(stack_a), NULL, BUILD_450) == FIBER_NULL);
    assert(init(fa.bytes + 4, "x", entry_a, 0, stack_a, sizeof(stack_a), NULL, BUILD_450) ==
           FIBER_ALIGNMENT);
    assert(init(&fa, "x", entry_a, 0, stack_a + 8, 4096, NULL, BUILD_450) == FIBER_ALIGNMENT);
    assert(init(&fa, "x", entry_a, 0, stack_a, 256, NULL, BUILD_450) == FIBER_RANGE);
    assert(init(&fa, "x", entry_a, 0, stack_a, 4104, NULL, BUILD_450) == FIBER_INVALID);
    assert(init(&fa, "x", entry_a, 0, NULL, 4096, NULL, BUILD_450) == FIBER_INVALID);
    _Alignas(8) unsigned char bad_opt[8] = {0};
    assert(init(&fa, "x", entry_a, 0, stack_a, 4096, bad_opt, BUILD_450) == FIBER_INVALID);
    /* The object is validated before the caller (as the reference does). */
    assert(init(&fa, "x", entry_a, 0, stack_a, sizeof(stack_a), NULL, BUILD_450) == 0);
    uint64_t r;
    assert(fswitch(&fa, 0, &r) == FIBER_PERMISSION && to_thread(0, &r) == FIBER_PERMISSION);
    void* self;
    assert(get_self(&self) == FIBER_PERMISSION);
    FiberObject junk = {0};
    assert(run(&junk, 0, &r) == FIBER_INVALID && finalize(&junk) == FIBER_INVALID);
}

static uint64_t cross_result;
static void* cross_thread(void* unused) {
    (void)unused;
    assert(run(&fb, 11, &cross_result) == 0); /* b was suspended on the main thread */
    return NULL;
}

static void scheduling(void) {
    _Alignas(8) unsigned char opt[128];
    assert(opt_init(opt) == 0);
    assert(init(&fa, "fiber-a", entry_a, 1, stack_a, sizeof(stack_a), opt, BUILD_450) == 0);
    assert(init(&fb, "fiber-b", entry_b, 2, stack_b, sizeof(stack_b), NULL, BUILD_450) == 0);
    uint64_t r = 0;
    assert(run(&fa, 5, &r) == 0 && r == 100);
    assert(log_values[0] == 1 && log_values[1] == 5 && log_values[2] == 0x9fc0);
    assert(run(&fa, 7, &r) == 0 && r == 200); /* a resumes, switches to b, b returns */
    assert(log_values[3] == 7 && log_values[4] == 2 && log_values[5] == 55);
    assert(run(&fa, 9, &r) == 0 && r == 300); /* a resumes from Switch */
    assert(log_values[6] == 9);
    pthread_t t;
    assert(!pthread_create(&t, NULL, cross_thread, NULL) && !pthread_join(t, NULL));
    assert(cross_result == 400 && log_values[7] == 11);
    assert(log_count == 8);
}

static void lifecycle(void) {
    struct {
        uint64_t size;
        void* entry;
        uint64_t arg;
        void* context;
        uint64_t context_size;
        char name[32];
        uint64_t margin;
        unsigned char pad[48];
    } info;
    _Static_assert(sizeof(info) == 128, "OrbisFiberInfo is 128 bytes");
    memset(&info, 0, sizeof(info));
    assert(get_info(&fa, &info) == FIBER_INVALID); /* size not set */
    info.size = sizeof(info);
    assert(get_info(&fa, &info) == 0 && info.entry == (void*)entry_a && info.arg == 1);
    assert(info.context == stack_a && info.context_size == sizeof(stack_a));
    assert(!strcmp(info.name, "fiber-a") && info.margin == UINT64_MAX);
    assert(rename_fiber(&fa, "renamed-fiber-with-a-long-name-xyz") == 0);
    assert(get_info(&fa, &info) == 0 && !strcmp(info.name, "renamed-fiber-with-a-long-name-"));
    assert(finalize(&fa) == 0 && finalize(&fa) == FIBER_STATE);
    uint64_t r;
    assert(run(&fa, 0, &r) == FIBER_STATE);
}

/* Stress: two fibers ping-pong; each Run happens on a fresh thread; MXCSR survives switches. */
static FiberObject fc, fd;
static _Alignas(16) unsigned char stack_c[16384], stack_d[16384]; /* the game's 16 KB stacks */
enum { ROUNDS = 20, SWITCHES = 10000 };
static void ABI ping(uint64_t arg_init, uint64_t arg_run) {
    (void)arg_init;
    uint64_t n = arg_run, back = 0;
    for (;;) {
        uint32_t mxcsr = 0x9fc0 | 0x6000; /* round toward zero: must not leak into pong */
        __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
        for (int i = 0; i < SWITCHES; ++i) {
            assert(fswitch(&fd, n + 1, &back) == 0 && back == n + 2);
            uint32_t now;
            __asm__ volatile("stmxcsr %0" : "=m"(now));
            assert(now == mxcsr);
            n = back;
        }
        assert(to_thread(n, &n) == 0);
    }
}
static void ABI pong(uint64_t arg_init, uint64_t arg_run) {
    (void)arg_init;
    uint64_t n = arg_run;
    for (;;) {
        uint32_t now;
        __asm__ volatile("stmxcsr %0" : "=m"(now));
        assert(now == 0x9fc0); /* pong's own FPU state */
        assert(fswitch(&fc, n + 1, &n) == 0);
    }
}
static uint64_t stress_value;
static void* stress_round(void* unused) {
    (void)unused;
    assert(run(&fc, stress_value, &stress_value) == 0);
    return NULL;
}
static void stress(void) {
    assert(init(&fc, "ping", ping, 0, stack_c, sizeof(stack_c), NULL, BUILD_450) == 0);
    assert(init(&fd, "pong", pong, 0, stack_d, sizeof(stack_d), NULL, BUILD_450) == 0);
    stress_value = 0;
    for (int round = 0; round < ROUNDS; ++round) {
        pthread_t t;
        assert(!pthread_create(&t, NULL, stress_round, NULL) && !pthread_join(t, NULL));
    }
    assert(stress_value == (uint64_t)ROUNDS * SWITCHES * 2);
}

int main(void) {
    runtime_start(1);
    init = (Init)resolve("_sceFiberInitializeImpl");
    opt_init = (OptInit)resolve("sceFiberOptParamInitialize");
    finalize = (Finalize)resolve("sceFiberFinalize");
    run = (Run)resolve("sceFiberRun");
    fswitch = (Switch)resolve("sceFiberSwitch");
    to_thread = (ReturnToThread)resolve("sceFiberReturnToThread");
    get_info = (GetInfo)resolve("sceFiberGetInfo");
    rename_fiber = (Rename)resolve("sceFiberRename");
    get_self = (GetSelf)resolve("sceFiberGetSelf");
    arguments();
    scheduling();
    lifecycle();
    stress();
    puts("PASS: fibers (arguments, run/return, switch, cross-thread resume, info, finalize, 400k "
         "switches)");
    return 0;
}
