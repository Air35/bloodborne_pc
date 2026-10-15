// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: small libkernel/libScePosix functions Shadow of the Colossus imports and bbport lacked
 * (runtime_sotc_kernel.c). Values: ADR-0009 (base PS4) and shadPS4 b4e7ae7
 * src/core/libraries/kernel/threads/pthread.h:32-40 (priority ranges), consulted only. */
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

static void* resolve(const char* name, const char* library) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#%s", nid, library);
    return (void*)runtime_resolve(key, 0);
}

typedef int32_t(ABI* NoArgs)(void);
typedef int32_t(ABI* Uuid)(unsigned char*);
typedef int32_t(ABI* Seconds)(uint32_t);
typedef int32_t(ABI* Policy)(int32_t);

static int32_t other_tid;
static void* tid_thread(void* arg) {
    other_tid = ((NoArgs)arg)();
    return NULL;
}

int main(void) {
    runtime_start(1);
    /* The names below must hash to the NIDs the game imports (--check-imports, plan A1). */
    static const char* const expected[][2] = {
        {"sceKernelUuidCreate", "Xjoosiw+XPI"},    {"sceKernelSleep", "-ZR+hG7aDHw"},
        {"sched_get_priority_max", "CBNtXOoef-E"}, {"sched_get_priority_min", "m0iS6jNsXds"},
        {"scePthreadGetthreadid", "EI-5-jlq2dE"},  {"sceKernelIsNeoMode", "WslcK1FQcGI"},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        char nid[12];
        assert(runtime_nid(expected[i][0], nid) && !strcmp(nid, expected[i][1]));
    }
    /* NID of sceKernelIsNeoMode is fixed by the SDK (docs/02 §4). */
    NoArgs neo = (NoArgs)runtime_resolve("WslcK1FQcGI#libkernel", 0);
    assert(neo && neo() == 0);

    Uuid uuid = (Uuid)resolve("sceKernelUuidCreate", "libkernel");
    unsigned char a[16], b[16];
    assert(uuid && uuid(NULL) == (int32_t)0x80020016);
    assert(uuid(a) == 0 && uuid(b) == 0 && memcmp(a, b, 16));
    assert((a[6] >> 4) == 4 && (a[8] & 0xc0) == 0x80); /* RFC 4122 version 4, variant 1 */

    Seconds sleep_s = (Seconds)resolve("sceKernelSleep", "libkernel");
    assert(sleep_s && sleep_s(0) == 0);

    Policy max = (Policy)resolve("sched_get_priority_max", "libScePosix");
    Policy min = (Policy)resolve("sched_get_priority_min", "libScePosix");
    assert(max && min);
    assert(max(1) == 767 && min(1) == 256 && max(3) == 767 && min(3) == 256);
    assert(max(2) == 22 && min(0) == 22); /* POSIX EINVAL for SCHED_OTHER/unknown */

    NoArgs tid = (NoArgs)resolve("scePthreadGetthreadid", "libkernel");
    NoArgs posix_tid = (NoArgs)resolve("pthread_getthreadid_np", "libScePosix");
    assert(tid && posix_tid && tid() > 0 && tid() == posix_tid());
    pthread_t t;
    assert(!pthread_create(&t, NULL, tid_thread, (void*)tid) && !pthread_join(t, NULL));
    assert(other_tid > 0 && other_tid != tid());
    puts("PASS: base-PS4 mode, UUID v4, sleep, priority ranges, thread IDs");
    return 0;
}
