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
#include <unistd.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

/* stderr capture: capture_begin sends stderr to a temporary file; capture_end restores it and
 * copies what was written into out (NUL-terminated). */
typedef struct {
    FILE* sink;
    int saved;
} Capture;
static Capture capture_begin(void) {
    Capture c = {tmpfile(), -1};
    assert(c.sink);
    fflush(stderr);
    c.saved = dup(STDERR_FILENO);
    assert(c.saved >= 0 && dup2(fileno(c.sink), STDERR_FILENO) >= 0);
    return c;
}
static void capture_end(Capture c, char* out, size_t size) {
    fflush(stderr);
    assert(dup2(c.saved, STDERR_FILENO) >= 0 && !close(c.saved));
    rewind(c.sink);
    const size_t n = fread(out, 1, size - 1, c.sink);
    out[n] = '\0';
    fclose(c.sink);
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
typedef void*(ABI* Self)(void);
typedef int32_t(ABI* GetSched)(void*, int32_t*, int32_t*);
typedef int32_t(ABI* SetSched)(void*, int32_t, const int32_t*);
typedef int32_t(ABI* Cancel)(void*);
typedef int32_t(ABI* AvailableDirect)(uint64_t, uint64_t, uint64_t, uint64_t*, uint64_t*);
typedef int32_t(ABI* AvailableFlexible)(uint64_t*);
typedef int32_t(ABI* Allocate)(int64_t, int64_t, uint64_t, uint64_t, int32_t, int64_t*);
typedef int32_t(ABI* Release)(int64_t, uint64_t);
typedef int32_t(ABI* MapFlexible)(void**, uint64_t, int32_t, int32_t);
typedef int32_t(ABI* Unmap)(void*, uint64_t);
typedef struct {
    int64_t t;
    uint32_t west_sec, dst_sec;
} Timesec;
typedef struct {
    int32_t minuteswest, dsttime;
} Timezone;
typedef int32_t(ABI* UtcToLocal)(int64_t, int64_t*, Timesec*, uint64_t*);
typedef int32_t(ABI* LocalToUtc)(int64_t, int64_t, int64_t*, Timezone*, int32_t*);

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

    /* pthread_{get,set}schedparam: shadPS4 b4e7ae7 threads/pthread.cpp:539-593 (consulted). */
    Self self = (Self)runtime_resolve("aI+OeCz8xrQ#libkernel", 0);
    GetSched get_sched = (GetSched)resolve("pthread_getschedparam", "libScePosix");
    SetSched set_sched = (SetSched)resolve("pthread_setschedparam", "libScePosix");
    assert(self && get_sched && set_sched);
    int32_t policy = -1, prio = -1, junk = 0;
    assert(get_sched(self(), &policy, &prio) == 0 && policy == 1 && prio == 700);
    assert(get_sched(self(), NULL, &prio) == 22 && get_sched(self(), &policy, NULL) == 22);
    assert(get_sched(&junk, &policy, &prio) == 3); /* ESRCH */
    const int32_t new_prio = 500;
    assert(set_sched(self(), 3, &new_prio) == 0);
    assert(get_sched(self(), &policy, &prio) == 0 && policy == 3 && prio == 500);
    assert(set_sched(self(), 3, NULL) == 22 && set_sched(self(), 9, &new_prio) == 22);
    assert(set_sched(&junk, 3, &new_prio) == 3);

    Cancel cancel = (Cancel)resolve("pthread_cancel", "libScePosix");
    assert(cancel && cancel(&junk) == 3);
    /* Recorded, not acted on; the line follows docs/05 §8.2: <ms> <LEVEL> <layer>/<comp> <event>
     * key=value. */
    char line[256];
    double ms = -1;
    char rest[200];
    Capture capture = capture_begin();
    const int32_t cancelled = cancel(self());
    capture_end(capture, line, sizeof(line));
    assert(cancelled == 0);
    assert(sscanf(line, "%lf WARN  L3/thread pthread_cancel_ignored thread=%199s", &ms, rest) == 2);
    assert(ms >= 0 && strchr(line, '\n') == line + strlen(line) - 1);

    /* Available memory: largest free aligned gap (shadPS4 b4e7ae7 core/memory.cpp:1235-1280). */
    AvailableDirect avail =
        (AvailableDirect)resolve("sceKernelAvailableDirectMemorySize", "libkernel");
    AvailableFlexible avail_flex =
        (AvailableFlexible)resolve("sceKernelAvailableFlexibleMemorySize", "libkernel");
    Allocate allocate = (Allocate)runtime_resolve("rTXw65xmLIA#libkernel", 0);
    Release release = (Release)runtime_resolve("MBuItvba6z8#libkernel", 0);
    MapFlexible map_flex = (MapFlexible)resolve("sceKernelMapFlexibleMemory", "libkernel");
    Unmap unmap = (Unmap)runtime_resolve("cQke9UuBQOk#libkernel", 0);
    assert(avail && avail_flex && allocate && release && map_flex && unmap);
    const uint64_t page = 16384, window = 64 * page;
    uint64_t at = 0, size = 0;
    assert(avail(0, window, page, NULL, &size) == (int32_t)0x80020016);
    assert(avail(0, window, page, &at, &size) == 0 && at == 0 && size == window);
    int64_t block_a = -1, block_b = -1;
    assert(allocate(0, (int64_t)window, 4 * page, page, 0, &block_a) == 0 && block_a == 0);
    assert(allocate(8 * page, (int64_t)window, 4 * page, page, 0, &block_b) == 0 &&
           block_b == (int64_t)(8 * page));
    /* Free: [4,8) and [12,64) pages; the largest wins. */
    assert(avail(0, window, page, &at, &size) == 0 && at == 12 * page && size == 52 * page);
    /* Range limits trim the gaps; alignment moves the start. */
    assert(avail(0, 10 * page, page, &at, &size) == 0 && at == 4 * page && size == 4 * page);
    assert(avail(0, window, 32 * page, &at, &size) == 0 && at == 32 * page && size == 32 * page);
    assert(avail(0, 4 * page, page, &at, &size) == (int32_t)0x8002000c); /* nothing free */
    assert(release(block_a, 4 * page) == 0 && release(block_b, 4 * page) == 0);

    uint64_t flex_before = 0, flex_after = 0;
    assert(avail_flex(NULL) == (int32_t)0x80020016);
    assert(avail_flex(&flex_before) == 0 && flex_before == UINT64_C(448) * 1024 * 1024);
    void* flex = NULL;
    assert(map_flex(&flex, 4 * page, 3, 0) == 0);
    assert(avail_flex(&flex_after) == 0 && flex_after == flex_before - 4 * page);
    assert(unmap(flex, 4 * page) == 0);

    /* Time zone conversions with POSIX TZ strings (no tzdata needed). Contract: shadPS4 b4e7ae7
     * kernel/time.cpp:457-530 (consulted); its DST sum and its LocaltimeToUtc sign are not
     * followed (see runtime_sotc_kernel.c), so the expectations are the arithmetic itself. */
    UtcToLocal to_local = (UtcToLocal)resolve("sceKernelConvertUtcToLocaltime", "libkernel");
    LocalToUtc to_utc = (LocalToUtc)resolve("sceKernelConvertLocaltimeToUtc", "libkernel");
    assert(to_local && to_utc);
    const int64_t january = 1705000000, july = 1720000000; /* 2024-01-11, 2024-07-03 (UTC) */
    int64_t local = 0, utc = 0;
    Timesec st = {0};
    uint64_t dst = 99;
    Timezone zone = {0};
    int32_t dst_seconds = 99;
    setenv("TZ", "UTC+3", 1); /* 3 h west, no DST */
    tzset();
    assert(to_local(january, &local, &st, &dst) == 0 && local == january - 3 * 3600);
    assert(st.t == january && (int32_t)st.west_sec == -3 * 3600 && st.dst_sec == 0 && dst == 0);
    assert(to_utc(local, 0, &utc, &zone, &dst_seconds) == 0 && utc == january);
    assert(zone.minuteswest == 180 && zone.dsttime == 0 && dst_seconds == 0);
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    assert(to_local(january, &local, NULL, NULL) == 0 && local == january - 5 * 3600);
    assert(to_local(july, &local, &st, &dst) == 0 && local == july - 4 * 3600 && dst == 3600);
    assert(st.dst_sec == 3600 && (int32_t)st.west_sec == -4 * 3600);
    assert(to_utc(local, 0, &utc, &zone, &dst_seconds) == 0 && utc == july);
    assert(zone.minuteswest == 240 && dst_seconds == 3600);
    assert(to_local(july, NULL, &st, &dst) == (int32_t)0x80020016);
    assert(to_utc(local, 0, &utc, NULL, &dst_seconds) == (int32_t)0x80020016);
    unsetenv("TZ");
    tzset();
    puts("PASS: base-PS4 mode, UUID v4, sleep, priority ranges, thread IDs, schedparam, cancel, "
         "available memory, time zone conversion");
    return 0;
}
