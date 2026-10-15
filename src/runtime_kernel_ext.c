// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: small libkernel/libScePosix functions the games' libraries import and the base port
 * lacked (--check-imports, docs/plans/2026-10-14-a1-imports-nid-biblioteca.md). Title-agnostic
 * (ADR-0016): per-title values are set by the profile through runtime_set_*. Resolved by name
 * (runtime_nid.c), so the same entry serves libkernel and libScePosix spellings.
 * References (consulted, reimplemented): ADR-0009 (sceKernelIsNeoMode = 0, base PS4);
 * shadPS4 b4e7ae7 src/core/libraries/kernel/threads/pthread.h:32-40 (priority ranges),
 * pthread.cpp:479-491 (sched_get_priority_*), kernel.cpp:169-185 (UUID). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/random.h>

#include "runtime.h"

enum { POSIX_EINVAL = 22 };                          /* FreeBSD errno numbers */
#define ORBIS_EINVAL ((int32_t)UINT32_C(0x80020016)) /* 0x80020000 | EINVAL */
enum { SCHED_FIFO_PS4 = 1, SCHED_RR_PS4 = 3 };       /* FreeBSD sched policies */
enum { PRIO_FIFO_LOWEST = 256, PRIO_FIFO_HIGHEST = 767 };

/* ADR-0009: the neutral default is a base PS4 (no Pro paths). A game profile may push another
 * value through runtime_set_neo_mode (ADR-0016: the core never asks which title is running).
 * Module state behind an accessor (docs/05 §4); set once at start, before guest threads. */
static atomic_int neo_mode;

void runtime_set_neo_mode(int enabled) {
    atomic_store_explicit(&neo_mode, enabled != 0, memory_order_relaxed);
}

static ABI int32_t is_neo_mode(void) {
    return atomic_load_explicit(&neo_mode, memory_order_relaxed);
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

/* sceKernelConvertUtcToLocaltime (-o5uEDpN+oY) and sceKernelConvertLocaltimeToUtc (0NTHN1NKONI),
 * both #libkernel; no direct call in the SotC eboot (plan A3 lote 5), so the callers are linked
 * modules [a verificar: which]. Argument layout from shadPS4 b4e7ae7 kernel/time.cpp:457-530
 * (consulted). The reference is inconsistent: it adds the DST amount on top of an offset that
 * already includes it, and LocaltimeToUtc subtracts minuteswest where west is positive. Here the
 * zone comes from the host (localtime_r/mktime, so per-date DST) and the arithmetic is plain:
 *   local = utc + gmtoff;  utc = mktime(local wall clock).
 * Timesec.west_sec keeps what both reference branches store (the offset east of UTC as u32, the
 * name says "west") [a verificar against the console]. */
typedef struct {
    int64_t t;
    uint32_t west_sec, dst_sec;
} Timesec;
typedef struct {
    int32_t minuteswest, dsttime;
} GuestTimezone;
enum { DST_SECONDS = 3600 }; /* the DST step is not reported by libc; one hour everywhere */

static ABI int32_t convert_utc_to_local(int64_t utc, int64_t* local, Timesec* st, uint64_t* dst) {
    if (!local)
        return ORBIS_EINVAL;
    const time_t at = (time_t)utc;
    struct tm tm;
    if (!localtime_r(&at, &tm))
        return ORBIS_EINVAL;
    const uint32_t dst_sec = tm.tm_isdst > 0 ? DST_SECONDS : 0;
    *local = utc + tm.tm_gmtoff;
    if (st) {
        st->t = utc;
        st->west_sec = (uint32_t)(int32_t)tm.tm_gmtoff;
        st->dst_sec = dst_sec;
    }
    if (dst)
        *dst = dst_sec;
    return 0;
}

/* `unused` is the second argument the reference carries without using it. */
static ABI int32_t convert_local_to_utc(int64_t local, int64_t unused, int64_t* utc,
                                        GuestTimezone* zone, int32_t* dst_seconds) {
    (void)unused;
    if (!zone)
        return ORBIS_EINVAL;
    const time_t wall = (time_t)local;
    struct tm tm;
    if (!gmtime_r(&wall, &tm))
        return ORBIS_EINVAL;
    tm.tm_isdst = -1;
    const time_t at = mktime(&tm);
    struct tm back;
    if (!localtime_r(&at, &back))
        return ORBIS_EINVAL;
    if (utc)
        *utc = (int64_t)at;
    zone->minuteswest = (int32_t)(-back.tm_gmtoff / 60);
    zone->dsttime = 0; /* folded into minuteswest, as runtime_kernel.c's sceKernelGettimezone */
    if (dst_seconds)
        *dst_seconds = back.tm_isdst > 0 ? DST_SECONDS : 0;
    return 0;
}

/* sceLibcHeapGetTraceInfo (NWtTN10cJzE#libSceLibcInternalExt): no reference implementation
 * (shadPS4 b4e7ae7 has only the NID name). Contract read from the caller, the libc heap init at
 * libc.prx +0x63df1 (plan A3 lote 6): it passes a 32-byte struct preloaded from a constant
 * {u32 size = 0x20, u32 0, u32 1, u32 0, ...}, ignores the result, then reads the u32 at +0xC
 * (non-zero turns heap tracing on) and the u64 at +0x10 and +0x18. Tracing is off here: the
 * fields past the header are zeroed, within the size the caller declared. Console behaviour for
 * a size below 0x10 or other versions of the struct is [a verificar]. */
static ABI int32_t libc_heap_trace_info(void* info) {
    enum { HEADER = 0x0c, FULL = 0x20 };
    if (!info)
        return ORBIS_EINVAL;
    uint32_t declared;
    memcpy(&declared, info, sizeof declared);
    const uint32_t end = declared < FULL ? declared : FULL;
    if (end > HEADER)
        memset((unsigned char*)info + HEADER, 0, end - HEADER);
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

/* Module records and C++ unwinding support, from gow3 91b6e61 src/runtime_kernel.c:167-183
 * (docs/auditorias/2026-10-14-gow3-pecas-a1-91b6e611d282.md). The runtime keeps no module
 * records; the game does not throw C++ exceptions (the SotC eboot imports no __cxa_throw nor
 * _Unwind_*, docs/plans/2026-10-14-a3-hle-lote2.md), so unwinding finds no frame data and
 * resuming a context is a stop, never a silent success. */
static ABI int32_t module_info_from_addr(uint64_t address, int32_t flags, void* info) {
    (void)address;
    if (flags >= 3)
        return (int32_t)UINT32_C(0x80020016); /* EINVAL */
    if (!info)
        return (int32_t)UINT32_C(0x8002000e); /* EFAULT */
    return (int32_t)UINT32_C(0x80020003);     /* ESRCH */
}
static ABI int32_t kernel_mlock(const void* address, uint64_t size) {
    (void)address;
    (void)size;
    return 0;
}
static ABI int32_t elf_phdr_match_addr(void* info, uint64_t address) {
    (void)info;
    (void)address;
    return 0;
}
static ABI int32_t is_signal_return(uint64_t pc) {
    (void)pc;
    return 0;
}
static ABI void pthread_cxa_finalize(void* dso) {
    (void)dso;
}
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn) is not supported\n", stderr);
    runtime_report();
    exit(23);
}
/* Host memory is never paged out by the runtime: syncing a mapping has nothing to do. */
static ABI int32_t posix_msync(void* address, uint64_t size, int flags) {
    (void)address;
    (void)size;
    (void)flags;
    return 0;
}
static ABI int32_t address_sanitizer_enabled(void) {
    return 0;
} /* retail build */

/* POSIX sockets: no network (CLAUDE.md §1; same "cable unplugged" model as the Net library in
 * runtime_services.c). No socket is ever created, so every descriptor is invalid. errno uses
 * FreeBSD numbers. */
enum { GUEST_EBADF = 9, GUEST_EINVAL = 22, GUEST_ENETUNREACH = 51 };
static int32_t posix_fail(int32_t guest_errno) {
    *runtime_errno() = guest_errno;
    return -1;
}
static ABI int32_t posix_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return posix_fail(GUEST_ENETUNREACH);
}
static ABI int32_t posix_bad_descriptor(int fd) {
    (void)fd;
    return posix_fail(GUEST_EBADF);
}
/* select() with no descriptor set is the classic portable sleep; any set descriptor is invalid. */
typedef struct {
    int64_t sec, usec;
} GuestTimeval;
static int any_descriptor(const unsigned char* set, int nfds) {
    for (int fd = 0; set && fd < nfds && fd < 1024; ++fd)
        if (set[fd / 8] & (1u << (fd % 8)))
            return 1;
    return 0;
}
static ABI int32_t posix_select(int nfds, const unsigned char* readfds,
                                const unsigned char* writefds, const unsigned char* exceptfds,
                                const GuestTimeval* timeout) {
    if (nfds < 0)
        return posix_fail(GUEST_EINVAL);
    if (any_descriptor(readfds, nfds) || any_descriptor(writefds, nfds) ||
        any_descriptor(exceptfds, nfds))
        return posix_fail(GUEST_EBADF);
    if (!timeout || timeout->sec < 0 || timeout->usec < 0 || timeout->usec >= 1000000)
        return posix_fail(GUEST_EINVAL); /* would wait forever on nothing */
    struct timespec left = {(time_t)timeout->sec, (long)timeout->usec * 1000};
    while (nanosleep(&left, &left) && errno == EINTR) {
    }
    return 0;
}
/* Priority protocols (none, inherit, protect) are accepted; host mutexes keep their own. */
static ABI int32_t mutexattr_setprotocol(void** attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2)
        return POSIX_EINVAL;
    return 0;
}

static const RuntimeExport exports[] = {
    {"socket", posix_socket},
    {"bind", posix_bad_descriptor},
    {"connect", posix_bad_descriptor},
    {"listen", posix_bad_descriptor},
    {"accept", posix_bad_descriptor},
    {"recv", posix_bad_descriptor},
    {"send", posix_bad_descriptor},
    {"setsockopt", posix_bad_descriptor},
    {"shutdown", posix_bad_descriptor},
    {"select", posix_select},
    {"pthread_mutexattr_setprotocol", mutexattr_setprotocol},
    {"sceKernelIsNeoMode", is_neo_mode},
    {"sceKernelUuidCreate", uuid_create},
    {"sceKernelSleep", kernel_sleep},
    {"sceLibcHeapGetTraceInfo", libc_heap_trace_info},
    {"sceKernelConvertUtcToLocaltime", convert_utc_to_local},
    {"sceKernelConvertLocaltimeToUtc", convert_local_to_utc},
    {"sched_get_priority_max", priority_max},
    {"sched_get_priority_min", priority_min},
    {"scePthreadGetthreadid", thread_id},
    {"pthread_getthreadid_np", thread_id},
    {"sceKernelGetModuleInfoFromAddr", module_info_from_addr},
    {"sceKernelGetModuleInfoForUnwind", module_info_from_addr},
    {"sceKernelMlock", kernel_mlock},
    {"__elf_phdr_match_addr", elf_phdr_match_addr},
    {"_is_signal_return", is_signal_return},
    {"__pthread_cxa_finalize", pthread_cxa_finalize},
    {"sigreturn", unwind_resume},
    {"msync", posix_msync},
    {"sceKernelIsAddressSanitizerEnabled", address_sanitizer_enabled},
};

uintptr_t runtime_kernel_ext_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
