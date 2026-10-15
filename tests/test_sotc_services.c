// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: kernel module-support symbols and offline services that Shadow of the Colossus (or its
 * libc.prx/libSceFios2.prx) imports and bbport lacked. Contracts: gow3 91b6e61
 * src/runtime_kernel.c:167-183 and src/runtime_services.c (trophy release, safe area);
 * shadPS4 b4e7ae7 src/core/libraries/disc_map/disc_map.cpp:20-26 (consulted). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

static uintptr_t by_name(const char* name, const char* library) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#%s", nid, library);
    return runtime_resolve(key, 0);
}
#define GET(type, name, lib) ((type)by_name(name, lib))

typedef int32_t(ABI* Int0)(void);
typedef int32_t(ABI* Int1)(int64_t);
typedef int32_t(ABI* InfoFromAddr)(uint64_t, int32_t, void*);
typedef int32_t(ABI* Ptr2)(const void*, uint64_t);
typedef int32_t(ABI* Msync)(void*, uint64_t, int);
typedef int32_t(ABI* SafeArea)(float*);
typedef int32_t(ABI* DiscMap)(const char*, int64_t, int64_t, int32_t*, int32_t*, int32_t*);

#define KERNEL_ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))

static void kernel_support(void) {
    unsigned char info[512];
    InfoFromAddr from = GET(InfoFromAddr, "sceKernelGetModuleInfoFromAddr", "libkernel");
    InfoFromAddr unwind = GET(InfoFromAddr, "sceKernelGetModuleInfoForUnwind", "libkernel");
    assert(from && unwind);
    assert(from(0x1000, 3, info) == KERNEL_ERR(22) && from(0x1000, 0, NULL) == KERNEL_ERR(14));
    assert(from(0x1000, 0, info) == KERNEL_ERR(3)); /* no module records: ESRCH */
    assert(unwind(0x1000, 0, info) == KERNEL_ERR(3));
    Ptr2 mlock = GET(Ptr2, "sceKernelMlock", "libkernel");
    assert(mlock && mlock(info, sizeof(info)) == 0);
    Msync msync = GET(Msync, "msync", "libkernel");
    assert(msync && msync(info, sizeof(info), 0) == 0);
    Ptr2 phdr = GET(Ptr2, "__elf_phdr_match_addr", "libkernel");
    Int1 signal_return = GET(Int1, "_is_signal_return", "libkernel");
    Int0 asan = GET(Int0, "sceKernelIsAddressSanitizerEnabled", "libkernel");
    assert(phdr && phdr(info, 0x1000) == 0 && signal_return && signal_return(0x1000) == 0);
    assert(asan && asan() == 0);
    assert(by_name("__pthread_cxa_finalize", "libkernel") && by_name("sigreturn", "libkernel"));
    assert(by_name("_nanosleep", "libkernel") == by_name("nanosleep", "libkernel"));
}

static void services(void) {
    Int1 destroy_ctx = GET(Int1, "sceNpTrophyDestroyContext", "libSceNpTrophy");
    Int1 destroy_handle = GET(Int1, "sceNpTrophyDestroyHandle", "libSceNpTrophy");
    assert(destroy_ctx && destroy_handle);
    assert(destroy_ctx(1) == 0 && destroy_handle(2) == 0);
    assert(destroy_ctx(0) == (int32_t)0x80551604 && destroy_handle(-1) == (int32_t)0x80551604);

    float area[33] = {0};
    SafeArea safe = GET(SafeArea, "sceSystemServiceGetDisplaySafeAreaInfo", "libSceSystemService");
    assert(safe && safe(NULL) == (int32_t)0x80A10003 && safe(area) == 0 && area[0] == 1.0f);

    /* fJgP+wqifno: the SDK name is unknown (shadPS4 calls it Func_7C980FFB0AA27E7A). */
    DiscMap map = (DiscMap)runtime_resolve("fJgP+wqifno#libSceDiscMap", 0);
    int32_t flags = 7, r1 = 7, r2 = 7;
    assert(map && map("/app0/x", 0, 4096, &flags, &r1, &r2) == 0 && !flags && !r1 && !r2);

    static const char* const accepted[][2] = {
        {"sceScreenShotEnable", "libSceScreenShot"},
        {"sceScreenShotDisable", "libSceScreenShot"},
        {"sceVideoRecordingSetInfo", "libSceVideoRecording"},
        {"sceGameLiveStreamingEnableLiveStreaming", "libSceGameLiveStreaming"},
        {"sceNetCtlInit", "libSceNetCtl"},
        {"sceNetCtlTerm", "libSceNetCtl"},
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
        Int0 f = GET(Int0, accepted[i][0], accepted[i][1]);
        assert(f && f() == 0);
    }
    Int0 headers = GET(Int0, "sceHttpGetAllResponseHeaders", "libSceHttp");
    Int0 sock_info = GET(Int0, "sceNetGetSockInfo", "libSceNet");
    assert(headers && headers() == (int32_t)0x80431063);     /* HTTP network error */
    assert(sock_info && sock_info() == (int32_t)0x80410133); /* ENETUNREACH */
}

int main(void) {
    runtime_start(1);
    kernel_support();
    services();
    puts("PASS: kernel module-support symbols, offline services, DiscMap 7C98");
    return 0;
}
