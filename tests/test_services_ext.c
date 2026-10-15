// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: kernel module-support symbols and offline services that the games (or their
 * libc.prx/libSceFios2.prx) import and the base port lacked. Contracts: gow3 91b6e61
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

/* Lote 3: dialogs in progress-bar mode stay open until closed (gow3 91b6e61 dialog_open_progress,
 * dialog_close, dialog_progress, dialog_result, dialog_open_mode); other modes finish with OK. */
typedef int32_t(ABI* Ptr1)(const void*);
typedef int32_t(ABI* Progress)(uint32_t, uint32_t);
enum { NONE = 0, INITIALIZED = 1, RUNNING = 2, FINISHED = 3 };
#define NOT_RUNNING ((int32_t)0x80B8000B)
#define NOT_FINISHED ((int32_t)0x80B80005)
#define ARG_NULL ((int32_t)0x80B8000D)
#define PARAM_INVALID ((int32_t)0x80B8000A)
static void dialogs(void) {
    Int0 init = GET(Int0, "sceSaveDataDialogInitialize", "libSceSaveDataDialog");
    Ptr1 open = GET(Ptr1, "sceSaveDataDialogOpen", "libSceSaveDataDialog");
    Int0 status = GET(Int0, "sceSaveDataDialogGetStatus", "libSceSaveDataDialog");
    Int0 update = GET(Int0, "sceSaveDataDialogUpdateStatus", "libSceSaveDataDialog");
    Int0 close = GET(Int0, "sceSaveDataDialogClose", "libSceSaveDataDialog");
    Progress progress =
        GET(Progress, "sceSaveDataDialogProgressBarSetValue", "libSceSaveDataDialog");
    Ptr1 result = GET(Ptr1, "sceSaveDataDialogGetResult", "libSceSaveDataDialog");
    Int0 ready = GET(Int0, "sceSaveDataDialogIsReadyToDisplay", "libSceSaveDataDialog");
    Int0 used = GET(Int0, "sceCommonDialogIsUsed", "libSceCommonDialog");
    assert(init && open && status && update && close && progress && result && ready && used);
    assert(ready() == 1 && used() == 0 && init() == 0 && status() == INITIALIZED);
    unsigned char param[128] = {0}, out[64];
    uint32_t mode = 5; /* progress bar */
    memcpy(param + 52, &mode, 4);
    assert(open(NULL) == ARG_NULL && open(param) == 0 && status() == RUNNING);
    assert(progress(0, 50) == 0 && progress(1, 50) == PARAM_INVALID && update() == RUNNING);
    assert(result(out) == NOT_FINISHED);
    assert(close() == 0 && status() == FINISHED && close() == NOT_RUNNING);
    assert(progress(0, 60) == NOT_RUNNING);
    mode = 2; /* any other mode: answered at once, OK */
    memcpy(param + 52, &mode, 4);
    uint64_t user_data = 0x1122334455667788u;
    memcpy(param + 112, &user_data, 8);
    char dir[32] = "x";
    void* dir_ptr = dir;
    memset(out, 0xee, sizeof(out));
    memcpy(out + 16, &dir_ptr, 8);
    memset(out + 24, 0, 8);
    assert(open(param) == 0 && status() == FINISHED && result(out) == 0);
    uint32_t got[3];
    memcpy(got, out, 12);
    assert(got[0] == 2 && got[1] == 0 && got[2] == 1 && dir[0] == 0); /* mode, OK, button */
    assert(!memcmp(out + 32, &user_data, 8));

    Int0 msg_init = GET(Int0, "sceMsgDialogInitialize", "libSceMsgDialog");
    Ptr1 msg_open = GET(Ptr1, "sceMsgDialogOpen", "libSceMsgDialog");
    Int0 msg_close = GET(Int0, "sceMsgDialogClose", "libSceMsgDialog");
    Ptr1 msg_result = GET(Ptr1, "sceMsgDialogGetResult", "libSceMsgDialog");
    assert(msg_init && msg_open && msg_close && msg_result && msg_init() == 0);
    unsigned char msg[96] = {0};
    mode = 2; /* MsgDialog progress bar */
    memcpy(msg + 56, &mode, 4);
    assert(msg_open(msg) == 0 && msg_result(out) == NOT_FINISHED && msg_close() == 0);
    assert(msg_result(out) == 0);

    Int0 err_init = GET(Int0, "sceErrorDialogInitialize", "libSceErrorDialog");
    Ptr1 err_open = GET(Ptr1, "sceErrorDialogOpen", "libSceErrorDialog");
    Int0 err_status = GET(Int0, "sceErrorDialogUpdateStatus", "libSceErrorDialog");
    Int0 err_close = GET(Int0, "sceErrorDialogClose", "libSceErrorDialog");
    Int0 err_term = GET(Int0, "sceErrorDialogTerminate", "libSceErrorDialog");
    assert(err_init && err_open && err_status && err_close && err_term);
    int32_t err_param[4] = {16, (int32_t)0x80020003, 1, 0};
    assert(err_open(err_param) == (int32_t)0x80B80006); /* not initialized: INVALID_STATE */
    assert(err_init() == 0 && err_init() == (int32_t)0x80B80004);
    assert(err_open(NULL) == ARG_NULL && err_open(err_param) == 0 && err_status() == FINISHED);
    assert(err_close() == NOT_RUNNING && err_term() == 0 && err_status() == NONE);
}

typedef int32_t(ABI* Escape)(char*, uint64_t*, uint64_t, const char*);
typedef int32_t(ABI* Socket)(int, int, int);
typedef int32_t(ABI* Fd)(int);
typedef int32_t(ABI* Select)(int, void*, void*, void*, void*);
typedef int32_t(ABI* SetProtocol)(void**, int);
static void network_and_posix(void) {
    Escape escape = GET(Escape, "sceHttpUriEscape", "libSceHttp");
    char out[64];
    uint64_t need = 0;
    assert(escape && escape(out, &need, sizeof(out), NULL) == (int32_t)0x804311fe);
    assert(escape(NULL, &need, 0, "a b/~") == 0 && need == 10);
    assert(escape(out, &need, 5, "a b/~") == (int32_t)0x80431022);
    assert(escape(out, &need, sizeof(out), "a b/~") == 0 && !strcmp(out, "a%20b%2F~"));

    Socket sock = GET(Socket, "socket", "libScePosix");
    Fd close_sock = GET(Fd, "shutdown", "libScePosix");
    assert(sock && sock(2, 1, 0) == -1 && *runtime_errno() == 51);      /* ENETUNREACH */
    assert(close_sock && close_sock(3) == -1 && *runtime_errno() == 9); /* EBADF */
    Select sel = GET(Select, "select", "libScePosix");
    struct {
        int64_t sec, usec;
    } timeout = {0, 1000};
    assert(sel && sel(0, NULL, NULL, NULL, &timeout) == 0); /* no descriptors: a sleep */
    uint64_t set = 1u << 3;
    assert(sel(4, &set, NULL, NULL, &timeout) == -1 && *runtime_errno() == 9);

    SetProtocol protocol = GET(SetProtocol, "pthread_mutexattr_setprotocol", "libScePosix");
    void* attr = &attr;
    assert(protocol && protocol(&attr, 0) == 0 && protocol(&attr, 2) == 0);
    assert(protocol(&attr, 3) == 22 && protocol(NULL, 0) == 22);

    typedef int32_t(ABI * HandleArg)(int32_t, int64_t);
    HandleArg light = GET(HandleArg, "scePadSetLightBar", "libScePad");
    HandleArg mix = GET(HandleArg, "sceAudioOutSetMixLevelPadSpk", "libSceAudioOut");
    assert(light && light(77, 0) == (int32_t)0x80920003); /* no such pad handle */
    assert(mix && mix(77, 0) < 0);                        /* no such audio port */
    char nid[12], key[64];
    assert(runtime_nid("__progname", nid));
    snprintf(key, sizeof(key), "%s#libkernel", nid);
    const char* const* progname = (const char* const*)runtime_resolve(key, 1);
    assert(progname && !strcmp(*progname, "eboot.bin"));
}

/* Peripherals with no device behind them (runtime_peripherals.c, plan A3 lote 5). */
typedef int32_t(ABI* Gesture2)(int32_t, const void*);
typedef int32_t(ABI* GestureIndex)(int32_t, const void*, uint32_t, void*);
typedef void(ABI* ImeHandler)(void*, const void*);
typedef int32_t(ABI* KeyboardOpen)(int32_t, const void*);
typedef int32_t(ABI* KeyboardClose)(int32_t);
typedef int32_t(ABI* ImeUpdate)(ImeHandler);
typedef struct {
    uint32_t option;
    int8_t reserved1[4];
    void* arg;
    ImeHandler handler;
    int8_t reserved2[8];
} KeyboardParam; /* shadPS4 b4e7ae7 ime/ime_common.h (OrbisImeKeyboardParam) */
static ABI void ime_handler(void* arg, const void* event) {
    (void)arg;
    (void)event;
    abort(); /* no keyboard: no event may reach the game */
}

static void gestures(void) {
    static const char* const names[] = {"sceSystemGestureOpen",
                                        "sceSystemGestureClose",
                                        "sceSystemGestureCreateTouchRecognizer",
                                        "sceSystemGestureUpdateTouchRecognizer",
                                        "sceSystemGestureInitializePrimitiveTouchRecognizer",
                                        "sceSystemGestureUpdatePrimitiveTouchRecognizer",
                                        "sceSystemGestureFinalizePrimitiveTouchRecognizer"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        Gesture2 f = GET(Gesture2, names[i], "libSceSystemGesture");
        assert(f && f(0, NULL) == 0);
    }
    Gesture2 count = GET(Gesture2, "sceSystemGestureGetTouchEventsCount", "libSceSystemGesture");
    GestureIndex by_index =
        GET(GestureIndex, "sceSystemGestureGetTouchEventByIndex", "libSceSystemGesture");
    unsigned char event[64] = {0xAA};
    assert(count && count(0, NULL) == 0); /* eboot+0xa2c232: jle skips the loop */
    assert(by_index && by_index(0, NULL, 0, event) == 0 && event[0] == 0xAA);
}

static void keyboard(void) {
    enum { USER = 1, USER_INVALID = -1 };
    const int32_t busy = (int32_t)0x80BC0001, not_opened = (int32_t)0x80BC0002;
    const int32_t bad_user = (int32_t)0x80BC0010, bad_handler = (int32_t)0x80BC0022;
    const int32_t bad_address = (int32_t)0x80BC0031, bad_reserved = (int32_t)0x80BC0032;
    KeyboardOpen open = GET(KeyboardOpen, "sceImeKeyboardOpen", "libSceIme");
    KeyboardClose close = GET(KeyboardClose, "sceImeKeyboardClose", "libSceIme");
    ImeUpdate update = GET(ImeUpdate, "sceImeUpdate", "libSceIme");
    assert(open && close && update);
    KeyboardParam param = {.handler = ime_handler};
    assert(update(ime_handler) == not_opened && close(USER) == not_opened);
    assert(open(USER, NULL) == bad_address);
    param.handler = NULL;
    assert(open(USER, &param) == bad_handler);
    param.handler = ime_handler;
    assert(open(USER_INVALID, &param) == bad_user);
    param.reserved2[3] = 1;
    assert(open(USER, &param) == bad_reserved);
    param.reserved2[3] = 0;
    assert(open(USER, &param) == 0 && open(USER, &param) == busy);
    assert(update(ime_handler) == 0); /* no events: ime_handler would abort */
    assert(close(USER_INVALID) == bad_user && close(USER) == 0 && close(USER) == not_opened);
}

static void share(void) {
    Int0 init = GET(Int0, "sceShareUtilityInitializeEx2", "libSceShareUtility");
    Int0 menu = GET(Int0, "sceShareUtilityOpenShareMenuDefault", "libSceShareUtility");
    assert(init && init() == 0 && menu && menu() == 0);
}

int main(void) {
    runtime_start(1);
    kernel_support();
    services();
    dialogs();
    network_and_posix();
    gestures();
    keyboard();
    share();
    puts("PASS: kernel module-support symbols, offline services, DiscMap 7C98, peripherals "
         "without a device");
    return 0;
}
