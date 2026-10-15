// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: small libkernel/libScePosix functions Shadow of the Colossus imports and bbport lacked
 * (--check-imports, docs/plans/2026-10-14-a1-imports-nid-biblioteca.md). Resolved by name
 * (runtime_nid.c), so the same entry serves libkernel and libScePosix spellings.
 * References (consulted, reimplemented): ADR-0009 (sceKernelIsNeoMode = 0, base PS4);
 * shadPS4 b4e7ae7 src/core/libraries/kernel/threads/pthread.h:32-40 (priority ranges),
 * pthread.cpp:479-491 (sched_get_priority_*), kernel.cpp:169-185 (UUID). */
#define _GNU_SOURCE
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/random.h>

#include "runtime.h"

enum { POSIX_EINVAL = 22 };                          /* FreeBSD errno numbers */
#define ORBIS_EINVAL ((int32_t)UINT32_C(0x80020016)) /* 0x80020000 | EINVAL */
enum { SCHED_FIFO_PS4 = 1, SCHED_RR_PS4 = 3 };       /* FreeBSD sched policies */
enum { PRIO_FIFO_LOWEST = 256, PRIO_FIFO_HIGHEST = 767 };

/* ADR-0009: the game must see a base PS4 (no Pro paths). */
static ABI int32_t is_neo_mode(void) {
    return 0;
}

/* Random RFC 4122 version-4 UUID into a 16-byte guest buffer. */
static ABI int32_t uuid_create(unsigned char* uuid) {
    if (!uuid)
        return ORBIS_EINVAL;
    for (size_t done = 0; done < 16;) {
        const ssize_t n = getrandom(uuid + done, 16 - done, 0);
        if (n < 0 && errno != EINTR)
            return ORBIS_EINVAL;
        if (n > 0)
            done += (size_t)n;
    }
    uuid[6] = (unsigned char)((uuid[6] & 0x0f) | 0x40);
    uuid[8] = (unsigned char)((uuid[8] & 0x3f) | 0x80);
    return 0;
}

static ABI int32_t kernel_sleep(uint32_t seconds) {
    struct timespec left = {(time_t)seconds, 0};
    while (nanosleep(&left, &left) && errno == EINTR) {
    }
    return 0;
}

static int realtime_policy(int32_t policy) {
    return policy == SCHED_FIFO_PS4 || policy == SCHED_RR_PS4;
}
static ABI int32_t priority_max(int32_t policy) {
    return realtime_policy(policy) ? PRIO_FIFO_HIGHEST : POSIX_EINVAL;
}
static ABI int32_t priority_min(int32_t policy) {
    return realtime_policy(policy) ? PRIO_FIFO_LOWEST : POSIX_EINVAL;
}

/* Unique per host thread for the thread's lifetime, like the PS4 thread ID. */
static ABI int32_t thread_id(void) {
    return (int32_t)gettid();
}

static const RuntimeExport exports[] = {
    {"sceKernelIsNeoMode", is_neo_mode},      {"sceKernelUuidCreate", uuid_create},
    {"sceKernelSleep", kernel_sleep},         {"sched_get_priority_max", priority_max},
    {"sched_get_priority_min", priority_min}, {"scePthreadGetthreadid", thread_id},
    {"pthread_getthreadid_np", thread_id},
};

uintptr_t runtime_sotc_kernel_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
