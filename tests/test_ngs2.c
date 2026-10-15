// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libSceNgs2 muted fallback (runtime_ngs2.c, plan docs/plans/2026-10-15-a3-ngs2-mudo.md,
 * ADR-0015). Layouts and error codes: shadPS4 b4e7ae7 ngs2/ngs2.h, ngs2_impl.h, ngs2_error.h
 * (consulted). The 11 functions are called through the NIDs the eboot imports, so the test goes
 * through the same resolver the loader uses. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

typedef struct {
    uint64_t size;
    char name[16];
    uint32_t flags, max_grain_samples, num_grain_samples, sample_rate, reserved[6];
} SystemOption;
typedef struct {
    uint64_t size;
    char name[16];
    uint32_t flags, max_grain_samples, max_voices, max_input_delay_blocks, max_matrices, max_ports,
        reserved[20];
} RackOption;
typedef struct {
    void* host_buffer;
    uint64_t host_buffer_size;
    uint64_t reserved[5];
    uint64_t user_data;
} ContextBufferInfo;
typedef struct {
    void* buffer;
    uint64_t buffer_size;
    uint32_t waveform_type, num_channels;
} RenderBufferInfo;
typedef struct {
    int32_t(ABI* alloc)(ContextBufferInfo*);
    int32_t(ABI* free)(ContextBufferInfo*);
    uint64_t user_data;
} Allocator;
_Static_assert(sizeof(SystemOption) == 64,
               "system option is 64 bytes (reference checks size == 64)");
_Static_assert(sizeof(RackOption) == 128, "rack option layout");
_Static_assert(sizeof(ContextBufferInfo) == 64, "context buffer info layout");
_Static_assert(sizeof(RenderBufferInfo) == 24, "render buffer info layout");

typedef int32_t(ABI* SystemCreate)(const SystemOption*, const Allocator*, uint64_t*);
typedef int32_t(ABI* Destroy)(uint64_t, ContextBufferInfo*);
typedef int32_t(ABI* Render)(uint64_t, const RenderBufferInfo*, uint32_t);
typedef int32_t(ABI* RackCreate)(uint64_t, uint32_t, const RackOption*, const ContextBufferInfo*,
                                 uint64_t*);
typedef int32_t(ABI* RackCreateAlloc)(uint64_t, uint32_t, const RackOption*, const Allocator*,
                                      uint64_t*);
typedef int32_t(ABI* VoiceHandle)(uint64_t, uint32_t, uint64_t*);
typedef int32_t(ABI* Query)(uint32_t, const RackOption*, ContextBufferInfo*);
typedef int32_t(ABI* Control)(uint64_t, const void*);
typedef int32_t(ABI* State)(uint64_t, void*, uint64_t);
typedef int32_t(ABI* Flags)(uint64_t, uint32_t*);

#define FAIL ((int32_t)0x804A0001)
#define INVALID_MAX_GRAIN ((int32_t)0x804A0050)
#define INVALID_NUM_GRAIN ((int32_t)0x804A0051)
#define INVALID_OUT ((int32_t)0x804A0053)
#define INVALID_OPTION_SIZE ((int32_t)0x804A0081)
#define INVALID_SAMPLE_RATE ((int32_t)0x804A0201)
#define INVALID_BUFFER_INFO ((int32_t)0x804A0206)
#define INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define INVALID_ALLOCATOR ((int32_t)0x804A020A)
#define INVALID_SYSTEM ((int32_t)0x804A0230)
#define INVALID_RACK ((int32_t)0x804A0261)
#define INVALID_VOICE ((int32_t)0x804A0300)
#define INVALID_VOICE_INDEX ((int32_t)0x804A0302)

static void* resolve(const char* name) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#libSceNgs2", nid);
    return (void*)runtime_resolve(key, 0);
}

static unsigned allocator_calls;
static int32_t ABI guest_alloc(ContextBufferInfo* info) {
    (void)info;
    ++allocator_calls;
    return 0;
}
static int32_t ABI guest_free(ContextBufferInfo* info) {
    (void)info;
    ++allocator_calls;
    return 0;
}

static SystemCreate system_create;
static Destroy system_destroy, rack_destroy;
static Render render;
static RackCreate rack_create;
static RackCreateAlloc rack_create_alloc;
static VoiceHandle voice_handle;
static Query query;
static Control control;
static State state;
static Flags flags;

static void bind(void) {
    system_create = (SystemCreate)resolve("sceNgs2SystemCreateWithAllocator");
    system_destroy = (Destroy)resolve("sceNgs2SystemDestroy");
    render = (Render)resolve("sceNgs2SystemRender");
    rack_create = (RackCreate)resolve("sceNgs2RackCreate");
    rack_create_alloc = (RackCreateAlloc)resolve("sceNgs2RackCreateWithAllocator");
    rack_destroy = (Destroy)resolve("sceNgs2RackDestroy");
    voice_handle = (VoiceHandle)resolve("sceNgs2RackGetVoiceHandle");
    query = (Query)resolve("sceNgs2RackQueryBufferSize");
    control = (Control)resolve("sceNgs2VoiceControl");
    state = (State)resolve("sceNgs2VoiceGetState");
    flags = (Flags)resolve("sceNgs2VoiceGetStateFlags");
    assert(system_create && system_destroy && render && rack_create && rack_create_alloc &&
           rack_destroy && voice_handle && query && control && state && flags);
}

static const Allocator allocator = {guest_alloc, guest_free, 0x1234};

static SystemOption good_system_option(void) {
    SystemOption o;
    memset(&o, 0, sizeof(o));
    o.size = sizeof(o);
    o.max_grain_samples = 512;
    o.num_grain_samples = 256;
    o.sample_rate = 48000;
    return o;
}

static void system_create_validation(void) {
    uint64_t handle = 0xabcd;
    assert(system_create(NULL, NULL, &handle) == INVALID_ALLOCATOR && handle == 0xabcd);
    const Allocator no_alloc = {NULL, guest_free, 0};
    assert(system_create(NULL, &no_alloc, &handle) == INVALID_ALLOCATOR);
    assert(system_create(NULL, &allocator, NULL) == INVALID_OUT);

    SystemOption o = good_system_option();
    o.size = 32;
    assert(system_create(&o, &allocator, &handle) == INVALID_OPTION_SIZE);
    o = good_system_option();
    o.max_grain_samples = 100; /* not a multiple of 64 */
    assert(system_create(&o, &allocator, &handle) == INVALID_MAX_GRAIN);
    o.max_grain_samples = 2048; /* above 1024 */
    assert(system_create(&o, &allocator, &handle) == INVALID_MAX_GRAIN);
    o = good_system_option();
    o.num_grain_samples = 0;
    assert(system_create(&o, &allocator, &handle) == INVALID_NUM_GRAIN);
    o = good_system_option();
    o.sample_rate = 12345;
    assert(system_create(&o, &allocator, &handle) == INVALID_SAMPLE_RATE);
    assert(handle == 0xabcd); /* no failure writes the handle */

    /* No option: the library defaults (512 / 256 / 48000) are valid. */
    assert(system_create(NULL, &allocator, &handle) == 0 && handle != 0);
    assert(system_destroy(handle, NULL) == 0);
    o = good_system_option();
    o.sample_rate = 44100;
    assert(system_create(&o, &allocator, &handle) == 0 && handle != 0);
    assert(system_destroy(handle, NULL) == 0);
    /* The fallback needs no memory: the game's allocator is never asked for any. */
    assert(allocator_calls == 0);
}

static void rack_and_voices(void) {
    uint64_t system = 0, rack = 0, voice0 = 0, voice1 = 0, again = 0;
    assert(system_create(NULL, &allocator, &system) == 0 && system);
    int local;

    RackOption ro;
    memset(&ro, 0, sizeof(ro));
    ro.size = sizeof(ro);
    ro.max_grain_samples = 512;
    ro.max_voices = 2;

    assert(rack_create_alloc(0, 0x1000, &ro, &allocator, &rack) == INVALID_SYSTEM);
    assert(rack_create_alloc((uint64_t)(uintptr_t)&local, 0x1000, &ro, &allocator, &rack) ==
           INVALID_SYSTEM);
    assert(rack_create_alloc(system, 0x1000, &ro, &allocator, NULL) == INVALID_OUT);
    assert(rack_create_alloc(system, 0x1000, &ro, NULL, &rack) == INVALID_ALLOCATOR);
    RackOption small = ro;
    small.size = 8;
    assert(rack_create_alloc(system, 0x1000, &small, &allocator, &rack) == INVALID_OPTION_SIZE);
    assert(rack == 0);
    assert(rack_create(system, 0x1000, &ro, NULL, &rack) == INVALID_BUFFER_INFO);
    assert(rack_create_alloc(system, 0x1000, &ro, &allocator, &rack) == 0 && rack);

    assert(voice_handle(0, 0, &voice0) == INVALID_RACK);
    assert(voice_handle(system, 0, &voice0) == INVALID_RACK); /* a system is not a rack */
    assert(voice_handle(rack, 0, NULL) == INVALID_OUT);
    assert(voice_handle(rack, 2, &voice0) == INVALID_VOICE_INDEX); /* maxVoices == 2 */
    assert(voice_handle(rack, 0, &voice0) == 0 && voice0);
    assert(voice_handle(rack, 1, &voice1) == 0 && voice1 && voice1 != voice0);
    assert(voice_handle(rack, 0, &again) == 0 && again == voice0); /* stable per index */

    /* Voices: always stopped, outputs written, nothing outside them. */
    struct {
        uint32_t value;
        uint32_t canary;
    } f = {0x55, 0xdeadbeef};
    assert(flags(voice0, &f.value) == 0 && f.value == 0 && f.canary == 0xdeadbeef);
    assert(flags(voice0, NULL) == INVALID_OUT);
    assert(flags(rack, &f.value) == INVALID_VOICE);
    assert(flags(0, &f.value) == INVALID_VOICE);
    unsigned char st[24];
    memset(st, 0xcc, sizeof(st));
    assert(state(voice1, st, 16) == 0);
    for (unsigned i = 0; i < 16; ++i)
        assert(st[i] == 0);
    for (unsigned i = 16; i < sizeof(st); ++i)
        assert(st[i] == 0xcc);
    assert(state(voice1, NULL, 16) == INVALID_OUT);
    assert(state(system, st, 16) == INVALID_VOICE);
    unsigned char params[16] = {0};
    assert(control(voice0, params) == 0);
    assert(control(voice0, NULL) == INVALID_BUFFER_ADDRESS);
    assert(control(0, params) == INVALID_VOICE);

    /* RackCreate (caller-provided buffer) gives a rack of the same kind. */
    ContextBufferInfo buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.host_buffer = &local;
    buffer.host_buffer_size = sizeof(local);
    uint64_t second = 0;
    assert(rack_create(system, 0x2000, NULL, &buffer, &second) == 0 && second && second != rack);

    /* Destroy: the buffer info comes back empty, handles die with their owner. */
    ContextBufferInfo out;
    memset(&out, 0xcc, sizeof(out));
    assert(rack_destroy(rack, &out) == 0);
    assert(out.host_buffer == NULL && out.host_buffer_size == 0 && out.user_data == 0);
    for (unsigned i = 0; i < 5; ++i)
        assert(out.reserved[i] == 0);
    assert(rack_destroy(rack, &out) == INVALID_RACK);
    assert(voice_handle(rack, 0, &again) == INVALID_RACK);
    assert(flags(voice0, &f.value) == INVALID_VOICE);
    assert(voice_handle(second, 0, &again) == 0); /* the other rack lives */

    assert(system_destroy(system, &out) == 0);
    assert(voice_handle(second, 0, &again) == INVALID_RACK);
    assert(system_destroy(system, &out) == INVALID_SYSTEM);
    assert(system_destroy(0, &out) == INVALID_SYSTEM);
    assert(rack_destroy(second, NULL) == INVALID_RACK);
}

static void query_and_render(void) {
    struct {
        ContextBufferInfo info;
        uint64_t canary;
    } q;
    memset(&q, 0xcc, sizeof(q));
    q.canary = 0x1122334455667788;
    assert(query(0x1000, NULL, NULL) == INVALID_BUFFER_ADDRESS);
    RackOption small;
    memset(&small, 0, sizeof(small));
    small.size = 8;
    assert(query(0x1000, &small, &q.info) == INVALID_OPTION_SIZE);
    assert(query(0x1000, NULL, &q.info) == 0);
    assert(q.info.host_buffer == NULL && q.info.host_buffer_size > 0);
    for (unsigned i = 0; i < 5; ++i)
        assert(q.info.reserved[i] == 0);
    assert(q.canary == 0x1122334455667788);

    uint64_t system = 0;
    assert(system_create(NULL, &allocator, &system) == 0);
    static unsigned char left[64], right[64];
    memset(left, 0xaa, sizeof(left));
    memset(right, 0xaa, sizeof(right));
    RenderBufferInfo infos[3] = {
        {left, 48, 3, 1},
        {NULL, 48, 3, 1}, /* nothing to write: skipped */
        {right, sizeof(right), 3, 2},
    };
    assert(render(system, infos, 3) == 0);
    for (unsigned i = 0; i < 48; ++i)
        assert(left[i] == 0);
    for (unsigned i = 48; i < sizeof(left); ++i)
        assert(left[i] == 0xaa); /* only bufferSize */
    for (unsigned i = 0; i < sizeof(right); ++i)
        assert(right[i] == 0);
    assert(render(system, NULL, 0) == 0);
    assert(render(system, NULL, 1) == INVALID_BUFFER_ADDRESS);
    assert(render(0, infos, 3) == INVALID_SYSTEM);
    assert(system_destroy(system, NULL) == 0);
    assert(render(system, infos, 3) == INVALID_SYSTEM);
}

static void capacity_is_bounded(void) {
    /* Fixed tables: running out is an error code, never memory growth or a crash. */
    uint64_t handles[64];
    unsigned made = 0;
    int32_t result = 0;
    while (made < 64 && (result = system_create(NULL, &allocator, &handles[made])) == 0)
        ++made;
    assert(made >= 1 && made < 64 && result == FAIL);
    for (unsigned i = 0; i < made; ++i)
        assert(system_destroy(handles[i], NULL) == 0);
    uint64_t again;
    assert(system_create(NULL, &allocator, &again) == 0); /* slots are reusable */
    assert(again != handles[0]);                          /* ... under a new generation */
    assert(system_destroy(again, NULL) == 0);
}

int main(void) {
    runtime_start(1);
    bind();
    system_create_validation();
    rack_and_voices();
    query_and_render();
    capacity_is_bounded();
    puts("PASS: Ngs2 muted fallback (validation, handles, voices, render silence, capacity)");
    return 0;
}
