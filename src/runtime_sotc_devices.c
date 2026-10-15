// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: peripherals Shadow of the Colossus imports that have no device behind them here
 * (docs/plans/2026-10-15-a3-hle-lote5.md): touchpad gestures, the USB keyboard of libSceIme and
 * the system share menu. Contracts consulted in shadPS4 b4e7ae7 (reimplemented, nothing copied):
 * src/core/libraries/system_gesture/system_gesture.cpp (every entry STUBBED, returns OK),
 * ime/ime.cpp:479-620 and 990-1015 (keyboard checks), ime/ime_common.h (error codes, param
 * layout). ShareUtility has no reference implementation (NID only). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "runtime.h"

/* docs/05 §8.2 line format; the fork has no central logger yet (plan 2026-10-15-a3-revisao-lote4).
 */
static void log_event(const char* level, const char* event) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "%.3f %-5s L3/devices %s\n",
            (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6, level, event);
}

/* --- libSceSystemGesture: no touchpad gestures --------------------------------------------- */

/* Open/Close and the recognizer calls succeed with handle 0, as the reference. The SotC opens at
 * eboot+0xa2bb3b (a negative result skips the feature). */
static ABI int32_t gesture_ok(void) {
    return 0;
}

/* No touch events ever: the call at eboot+0xa2c21f is followed by `test; jle` (eboot+0xa2c232)
 * that skips the event loop when the count is <= 0 [eboot disassembly, plan A3 lote 5]. */
static ABI int32_t gesture_events_count(void) {
    return 0;
}

/* The only call (eboot+0xa2c253) sits inside that loop, so it is never reached with a count of
 * 0; answers OK without writing, as the reference (no source for an error code). */
static ABI int32_t gesture_event_by_index(void) {
    log_event("WARN", "gesture_event_by_index_without_events");
    return 0;
}

/* --- libSceIme USB keyboard: none connected ------------------------------------------------- */

/* Codes: shadPS4 b4e7ae7 ime/ime_common.h (two readings agree). BUSY, INVALID_*: from that file
 * only. NOT_OPENED (0x80BC0002) and CONNECTION_FAILED (0x80BC0004) also appear as immediates in
 * the eboot at 0xa2b531/0xa2b53c: the game calls sceImeUpdate each frame, opens the keyboard on
 * NOT_OPENED (0xa2b5ee..0xa2b613, option=1 "Repeat") and closes it on CONNECTION_FAILED. */
#define IME_ERROR(n) ((int32_t)(UINT32_C(0x80BC0000) | (n)))
enum {
    IME_BUSY = 0x01,
    IME_NOT_OPENED = 0x02,
    IME_INVALID_USER_ID = 0x10,
    IME_INVALID_HANDLER = 0x22,
    IME_INVALID_ADDRESS = 0x31,
    IME_INVALID_RESERVED = 0x32,
};
enum { USER_ID_INVALID = -1 }; /* SCE_USER_SERVICE_USER_ID_INVALID */

typedef void(ABI* ImeEventHandler)(void* arg, const void* event);
typedef struct {
    uint32_t option;
    int8_t reserved1[4];
    void* arg;
    ImeEventHandler handler;
    int8_t reserved2[8];
} KeyboardParam; /* OrbisImeKeyboardParam; the game builds 32 zeroed bytes at 0xa2b5f2 */

/* Module state (docs/05 §4): the guest calls these entries with no context of its own, so the
 * one keyboard slot lives here, behind the accessors below. */
static struct {
    pthread_mutex_t lock;
    int open;
} keyboard = {PTHREAD_MUTEX_INITIALIZER, 0};

/* Sets the slot to `open` and returns the previous value. */
static int keyboard_exchange(int open) {
    pthread_mutex_lock(&keyboard.lock);
    const int previous = keyboard.open;
    keyboard.open = open;
    pthread_mutex_unlock(&keyboard.lock);
    return previous;
}

static int keyboard_is_open(void) {
    pthread_mutex_lock(&keyboard.lock);
    const int open = keyboard.open;
    pthread_mutex_unlock(&keyboard.lock);
    return open;
}

static int zeroed(const int8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (bytes[i])
            return 0;
    return 1;
}

/* Checks in the reference order. The option bits (OrbisImeKeyboardOption, u32: Repeat=1,
 * RepeatEachKey=2, AddOsk=4, EffectiveWithIme=8, DisableResume=16, DisableCapslock...=32) are
 * not validated: the reference builds its mask by a template [a verificar] and the SotC only
 * passes Repeat. */
static int32_t keyboard_param_error(int32_t user, const KeyboardParam* param) {
    if (!param)
        return IME_ERROR(IME_INVALID_ADDRESS);
    if (!param->handler)
        return IME_ERROR(IME_INVALID_HANDLER);
    if (user == USER_ID_INVALID)
        return IME_ERROR(IME_INVALID_USER_ID);
    if (!zeroed(param->reserved1, sizeof(param->reserved1)) ||
        !zeroed(param->reserved2, sizeof(param->reserved2)))
        return IME_ERROR(IME_INVALID_RESERVED);
    return 0;
}

static ABI int32_t keyboard_open_hle(int32_t user, const KeyboardParam* param) {
    const int32_t error = keyboard_param_error(user, param);
    if (error)
        return error;
    if (keyboard_exchange(1))
        return IME_ERROR(IME_BUSY);
    log_event("INFO", "ime_keyboard_open connected=0");
    return 0;
}

/* Reference order: nothing open first, then the user id. */
static ABI int32_t keyboard_close_hle(int32_t user) {
    if (!keyboard_is_open())
        return IME_ERROR(IME_NOT_OPENED);
    if (user == USER_ID_INVALID)
        return IME_ERROR(IME_INVALID_USER_ID);
    return keyboard_exchange(0) ? 0 : IME_ERROR(IME_NOT_OPENED);
}

/* Only the keyboard is served here (the fork has no sceImeOpen): with it open, there is simply
 * nothing to deliver to the handler. */
static ABI int32_t ime_update(ImeEventHandler handler) {
    (void)handler;
    return keyboard_is_open() ? 0 : IME_ERROR(IME_NOT_OPENED);
}

/* --- libSceShareUtility: no share menu ------------------------------------------------------ */

static ABI int32_t share_initialize(void) {
    return 0;
}

static ABI int32_t share_open_menu(void) {
    log_event("INFO", "share_menu_not_shown");
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceSystemGestureOpen", gesture_ok},
    {"sceSystemGestureClose", gesture_ok},
    {"sceSystemGestureCreateTouchRecognizer", gesture_ok},
    {"sceSystemGestureUpdateTouchRecognizer", gesture_ok},
    {"sceSystemGestureInitializePrimitiveTouchRecognizer", gesture_ok},
    {"sceSystemGestureUpdatePrimitiveTouchRecognizer", gesture_ok},
    {"sceSystemGestureFinalizePrimitiveTouchRecognizer", gesture_ok},
    {"sceSystemGestureGetTouchEventsCount", gesture_events_count},
    {"sceSystemGestureGetTouchEventByIndex", gesture_event_by_index},
    {"sceImeKeyboardOpen", keyboard_open_hle},
    {"sceImeKeyboardClose", keyboard_close_hle},
    {"sceImeUpdate", ime_update},
    {"sceShareUtilityInitializeEx2", share_initialize},
    {"sceShareUtilityOpenShareMenuDefault", share_open_menu},
};

uintptr_t runtime_sotc_devices_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
