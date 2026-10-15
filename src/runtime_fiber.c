// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libSceFiber, the job system of Shadow of the Colossus' engine (docs/02 §4).
 * Contracts from the PS4 SDK as implemented by shadPS4 b4e7ae7 src/core/libraries/fiber/
 * (fiber.h, fiber.cpp, fiber_error.h; consulted, reimplemented with an own design):
 * - the SceFiber object is 256 guest bytes, 8-aligned, opaque to the game: the runtime keeps
 *   its state there (magic words, state, entry, context, saved stack pointer);
 * - Run only from a thread, Switch/ReturnToThread only from a fiber (else PERMISSION);
 *   Idle -> Run by compare-and-swap, so a fiber never runs twice at once;
 * - a suspended fiber may be resumed by another thread: per-thread state is re-read after every
 *   switch through run_state() (noinline), never cached across sotc_fiber_swap.
 * Switching is plain register save/restore (callee-saved registers, rsp, MXCSR, x87 control
 * word) on the fiber's own context: no syscall per switch (docs/02 §5, Ajnc item 30).
 * Not yet: fibers without a context (run on the thread stack) stop with STOP:; stack overflow
 * detection; registration of fiber stacks for GPU page tracking (A3, docs/01 §4). */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define FIBER_NULL ((int32_t)0x80590001)
#define FIBER_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_RANGE ((int32_t)0x80590003)
#define FIBER_INVALID ((int32_t)0x80590004)
#define FIBER_PERMISSION ((int32_t)0x80590005)
#define FIBER_STATE ((int32_t)0x80590006)

enum { MAGIC_START = 0x534f5443u /* "SOTC" */, MAGIC_END = 0x46494252u /* "FIBR" */ };
enum { FIBER_RUN = 1, FIBER_IDLE = 2, FIBER_TERMINATED = 3 };
enum { CONTEXT_MINIMUM = 512, NAME_MAX_LENGTH = 31, OPT_MAGIC = 0xbb40e64du };
enum { FLAG_SET_FPU = 0x100 };
#define SDK_FPU_DEFAULTS 0x03500000u /* firmware 3.50: new fibers start with default FPU state */

typedef void(ABI* FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);

typedef struct Fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    unsigned char* context;
    uint64_t context_size;
    char name[NAME_MAX_LENGTH + 1];
    uint64_t saved_rsp; /* valid while the fiber is started and suspended */
    uint32_t flags, started;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber) <= 256, "SceFiber is 256 bytes");

typedef struct {
    uint64_t thread_rsp; /* the thread inside sceFiberRun */
    Fiber* current;      /* fiber running on this thread, NULL outside fibers */
    Fiber* pending_idle; /* fiber that just left its stack: Idle once off it */
    uint64_t transfer;   /* argument passed by the switch that resumed us */
} RunState;
static _Thread_local RunState run_tls;

/* Per-thread state, recomputed on every call: after a switch the code may be on another thread. */
static __attribute__((noinline)) RunState* run_state(void) {
    RunState* state = &run_tls;
    __asm__ volatile("" : "+r"(state));
    return state;
}

/* void sotc_fiber_swap(uint64_t *save_rsp, uint64_t load_rsp): saves the callee-saved state on
 * the current stack, stores rsp in *save_rsp, loads load_rsp and restores the state saved there. */
void sotc_fiber_swap(uint64_t* save_rsp, uint64_t load_rsp);
__asm__(".text\n"
        ".globl sotc_fiber_swap\n"
        ".hidden sotc_fiber_swap\n"
        ".type sotc_fiber_swap,@function\n"
        "sotc_fiber_swap:\n"
        "  push %rbp\n  push %rbx\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
        "  sub $16,%rsp\n  stmxcsr (%rsp)\n  fnstcw 4(%rsp)\n"
        "  mov %rsp,(%rdi)\n"
        "  mov %rsi,%rsp\n"
        "  ldmxcsr (%rsp)\n  fldcw 4(%rsp)\n  add $16,%rsp\n"
        "  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbx\n  pop %rbp\n"
        "  ret\n"
        ".size sotc_fiber_swap,.-sotc_fiber_swap\n");

/* The fiber that just left its stack can now be resumed by anyone. */
static void settle(RunState* run) {
    if (!run->pending_idle)
        return;
    atomic_store_explicit(&run->pending_idle->state, FIBER_IDLE, memory_order_release);
    run->pending_idle = NULL;
}

static __attribute__((noreturn)) void fiber_start(void) {
    RunState* run = run_state();
    settle(run);
    Fiber* fiber = run->current;
    fiber->entry(fiber->arg_on_initialize, run->transfer);
    fprintf(stderr, "STOP: fiber '%s' returned from its entry function\n", fiber->name);
    runtime_report();
    exit(21);
}

/* First switch into a fiber "returns" into fiber_start with an ABI-aligned stack. */
static void prepare_stack(Fiber* fiber) {
    uint32_t mxcsr = 0x9fc0;
    uint16_t fpucw = 0x037f;
    if (!(fiber->flags & FLAG_SET_FPU)) {
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        __asm__ volatile("fnstcw %0" : "=m"(fpucw));
    }
    uint64_t* sp = (uint64_t*)(((uintptr_t)fiber->context + fiber->context_size) & ~(uintptr_t)15);
    *--sp = 0; /* fiber_start's (absent) return address keeps rsp % 16 == 8 at its entry */
    *--sp = (uint64_t)(uintptr_t)fiber_start;
    for (int i = 0; i < 6; ++i)
        *--sp = 0; /* rbp, rbx, r12-r15 */
    sp -= 2;
    memcpy(sp, &mxcsr, 4);
    memcpy((unsigned char*)sp + 4, &fpucw, 2);
    fiber->saved_rsp = (uint64_t)(uintptr_t)sp;
    fiber->started = 1;
}

static int32_t check(const Fiber* fiber) {
    if (!fiber)
        return FIBER_NULL;
    if ((uintptr_t)fiber & 7)
        return FIBER_ALIGNMENT;
    if (fiber->magic_start != MAGIC_START || fiber->magic_end != MAGIC_END)
        return FIBER_INVALID;
    return 0;
}

/* Idle -> Run for a fiber about to be entered; contextless fibers are not supported yet. */
static int32_t claim(Fiber* fiber) {
    uint32_t idle = FIBER_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&fiber->state, &idle, FIBER_RUN,
                                                 memory_order_acquire, memory_order_relaxed))
        return FIBER_STATE;
    if (!fiber->context) {
        fprintf(stderr, "STOP: fiber '%s' has no context (thread-stack fibers unsupported)\n",
                fiber->name);
        runtime_report();
        exit(21);
    }
    if (!fiber->started)
        prepare_stack(fiber);
    return 0;
}

static int32_t validate_context(const void* context, uint64_t size) {
    if ((uintptr_t)context & 15)
        return FIBER_ALIGNMENT;
    if (size && size < CONTEXT_MINIMUM)
        return FIBER_RANGE;
    if ((size & 15) || (!context) != (!size))
        return FIBER_INVALID;
    return 0;
}

static ABI int32_t fiber_initialize(Fiber* fiber, const char* name, FiberEntry entry,
                                    uint64_t arg_on_initialize, void* context, uint64_t size,
                                    const uint32_t* opt_param, uint32_t build_version) {
    if (!fiber || !name || !entry)
        return FIBER_NULL;
    if ((uintptr_t)fiber & 7 || (opt_param && (uintptr_t)opt_param & 7))
        return FIBER_ALIGNMENT;
    const int32_t context_error = validate_context(context, size);
    if (context_error)
        return context_error;
    if (opt_param && *opt_param != OPT_MAGIC)
        return FIBER_INVALID;
    memset(fiber, 0, sizeof(*fiber));
    strncpy(fiber->name, name, NAME_MAX_LENGTH);
    fiber->entry = entry;
    fiber->arg_on_initialize = arg_on_initialize;
    fiber->context = context;
    fiber->context_size = size;
    fiber->flags = build_version >= SDK_FPU_DEFAULTS ? FLAG_SET_FPU : 0;
    fiber->magic_start = MAGIC_START;
    fiber->magic_end = MAGIC_END;
    atomic_store_explicit(&fiber->state, FIBER_IDLE, memory_order_release);
    return 0;
}

static ABI int32_t fiber_opt_param_initialize(uint32_t* opt_param) {
    if (!opt_param)
        return FIBER_NULL;
    if ((uintptr_t)opt_param & 7)
        return FIBER_ALIGNMENT;
    *opt_param = OPT_MAGIC;
    return 0;
}

static ABI int32_t fiber_finalize(Fiber* fiber) {
    const int32_t error = check(fiber);
    if (error)
        return error;
    uint32_t idle = FIBER_IDLE;
    return atomic_compare_exchange_strong(&fiber->state, &idle, FIBER_TERMINATED) ? 0 : FIBER_STATE;
}

static ABI int32_t fiber_run(Fiber* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_return) {
    const int32_t error = check(fiber);
    if (error)
        return error;
    RunState* run = run_state();
    if (run->current)
        return FIBER_PERMISSION;
    const int32_t claimed = claim(fiber);
    if (claimed)
        return claimed;
    run->current = fiber;
    run->transfer = arg_on_run_to;
    sotc_fiber_swap(&run->thread_rsp, fiber->saved_rsp);
    run = run_state();
    settle(run); /* the fiber that returned to this thread */
    run->current = NULL;
    if (arg_on_return)
        *arg_on_return = run->transfer;
    return 0;
}

static ABI int32_t fiber_switch(Fiber* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_run) {
    const int32_t error = check(fiber);
    if (error)
        return error;
    RunState* run = run_state();
    Fiber* self = run->current;
    if (!self)
        return FIBER_PERMISSION;
    const int32_t claimed = claim(fiber);
    if (claimed)
        return claimed;
    run->current = fiber;
    run->transfer = arg_on_run_to;
    run->pending_idle = self;
    sotc_fiber_swap(&self->saved_rsp, fiber->saved_rsp);
    run = run_state();
    settle(run);
    if (arg_on_run)
        *arg_on_run = run->transfer;
    return 0;
}

static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t* arg_on_run) {
    RunState* run = run_state();
    Fiber* self = run->current;
    if (!self)
        return FIBER_PERMISSION;
    run->transfer = arg_on_return;
    run->pending_idle = self;
    sotc_fiber_swap(&self->saved_rsp, run->thread_rsp);
    run = run_state();
    settle(run);
    if (arg_on_run)
        *arg_on_run = run->transfer;
    return 0;
}

static ABI int32_t fiber_get_self(Fiber** out) {
    if (!out)
        return FIBER_NULL;
    Fiber* self = run_state()->current;
    if (!self)
        return FIBER_PERMISSION;
    *out = self;
    return 0;
}

typedef struct {
    uint64_t size;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void* context;
    uint64_t context_size;
    char name[NAME_MAX_LENGTH + 1];
    uint64_t context_margin;
    unsigned char reserved[48];
} FiberInfo;
_Static_assert(sizeof(FiberInfo) == 128, "SceFiberInfo is 128 bytes");

static ABI int32_t fiber_get_info(Fiber* fiber, FiberInfo* info) {
    if (!fiber || !info)
        return FIBER_NULL;
    if ((uintptr_t)info & 7)
        return FIBER_ALIGNMENT;
    const int32_t error = check(fiber);
    if (error)
        return error;
    if (info->size != sizeof(*info))
        return FIBER_INVALID;
    info->entry = fiber->entry;
    info->arg_on_initialize = fiber->arg_on_initialize;
    info->context = fiber->context;
    info->context_size = fiber->context_size;
    memcpy(info->name, fiber->name, sizeof(info->name));
    info->context_margin = UINT64_MAX; /* no context size check */
    return 0;
}

static ABI int32_t fiber_rename(Fiber* fiber, const char* name) {
    if (!fiber || !name)
        return FIBER_NULL;
    const int32_t error = check(fiber);
    if (error)
        return error;
    memset(fiber->name, 0, sizeof(fiber->name));
    strncpy(fiber->name, name, NAME_MAX_LENGTH);
    return 0;
}

static const RuntimeExport exports[] = {
    /* hVYD7Ou2pCQ: named _sceFiberInitializeImpl in aerolib; sceFiberInitialize's signature
     * (build version last) per shadPS4 b4e7ae7 fiber.cpp RegisterLib. */
    {"_sceFiberInitializeImpl", fiber_initialize},
    {"sceFiberOptParamInitialize", fiber_opt_param_initialize},
    {"sceFiberFinalize", fiber_finalize},
    {"sceFiberRun", fiber_run},
    {"sceFiberSwitch", fiber_switch},
    {"sceFiberReturnToThread", fiber_return_to_thread},
    {"sceFiberGetSelf", fiber_get_self},
    {"sceFiberGetInfo", fiber_get_info},
    {"sceFiberRename", fiber_rename},
};

uintptr_t runtime_fiber_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
