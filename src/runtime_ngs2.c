// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libSceNgs2, muted fallback (docs/plans/2026-10-15-a3-ngs2-mudo.md, ADR-0015).
 *
 * The console's libSceNgs2 does the DSP (sampler, submixer, reverb, mastering); until that
 * module is provided to the host this file only keeps the handle graph so the title gets past
 * its audio initialization: systems, racks and voices exist, voices are always stopped and a
 * render writes silence. Nothing here decodes or mixes audio.
 *
 * Error codes and option validation follow shadPS4 b4e7ae7 src/core/libraries/ngs2/
 * ngs2_error.h, ngs2_impl.cpp (SystemSetup, SystemSetupCore, RackQueryBufferSize) and ngs2.cpp
 * (consulted, reimplemented). One deliberate difference: the reference returns OK without
 * writing handles or state, which would hand the game uninitialized memory; here every output
 * is written.
 *
 * Handles are opaque 64-bit values (kind, generation, slot) over fixed tables, never pointers:
 * a stale or foreign handle is rejected by lookup, and nothing is allocated. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "runtime.h"

#define NGS2_FAIL ((int32_t)0x804A0001)
#define NGS2_INVALID_MAX_GRAIN_SAMPLES ((int32_t)0x804A0050)
#define NGS2_INVALID_NUM_GRAIN_SAMPLES ((int32_t)0x804A0051)
#define NGS2_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define NGS2_INVALID_OPTION_SIZE ((int32_t)0x804A0081)
#define NGS2_INVALID_SAMPLE_RATE ((int32_t)0x804A0201)
#define NGS2_INVALID_BUFFER_INFO ((int32_t)0x804A0206)
#define NGS2_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define NGS2_INVALID_BUFFER_ALLOCATOR ((int32_t)0x804A020A)
#define NGS2_INVALID_SYSTEM_HANDLE ((int32_t)0x804A0230)
#define NGS2_INVALID_RACK_HANDLE ((int32_t)0x804A0261)
#define NGS2_INVALID_VOICE_HANDLE ((int32_t)0x804A0300)
#define NGS2_INVALID_VOICE_INDEX ((int32_t)0x804A0302)

/* Layouts (ngs2.h, ngs2_impl.h of the reference). */
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
    void* alloc_handler;
    void* free_handler;
    uint64_t user_data;
} BufferAllocator;

enum {
    SYSTEM_OPTION_SIZE = 64,
    MAX_SYSTEMS = 8,
    MAX_RACKS = 64,
    /* Voices have no storage; this only bounds RackGetVoiceHandle for racks that gave no
     * maxVoices. [a verificar: the console's default maxVoices] */
    MAX_VOICES = 1024,
    /* Placeholder for RackQueryBufferSize: non-zero so a caller that allocates exactly this
     * does not see a zero-size request fail. [a verificar: the console's value] */
    RACK_BUFFER_BYTES = 256,
    /* Sanity bound for the state buffer a caller declares. */
    MAX_STATE_BYTES = 4096,
};

enum { KIND_SYSTEM = 1, KIND_RACK = 2, KIND_VOICE = 3 };

typedef struct {
    uint32_t generation; /* 0: slot free; handles carry it, so a reused slot is a new handle */
} SystemSlot;
typedef struct {
    uint32_t generation;
    uint32_t system_slot, system_generation;
    uint32_t voices;
} RackSlot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SystemSlot systems[MAX_SYSTEMS];
static RackSlot racks[MAX_RACKS];
static uint32_t next_generation; /* shared counter: handles never repeat within a run */
static int announced;

static void log_event(const char* level, const char* event) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "%.3f %-5s L3/ngs2 %s\n", (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6,
            level, event);
}

static uint64_t encode(unsigned kind, uint32_t generation, uint32_t slot, uint32_t extra) {
    return (uint64_t)kind << 60 | (uint64_t)(generation & 0x0fffffffu) << 32 |
           (uint64_t)extra << 16 | slot;
}
static unsigned kind_of(uint64_t handle) {
    return (unsigned)(handle >> 60);
}
static uint32_t generation_of(uint64_t handle) {
    return (uint32_t)(handle >> 32) & 0x0fffffffu;
}
static uint32_t slot_of(uint64_t handle) {
    return (uint32_t)handle & 0xffffu;
}
static uint32_t extra_of(uint64_t handle) {
    return (uint32_t)(handle >> 16) & 0xffffu;
}
static uint32_t new_generation(void) {
    next_generation = (next_generation + 1) & 0x0fffffffu;
    return next_generation ? next_generation : ++next_generation;
}

/* Lookups run under `lock`; they return the slot or NULL. */
static SystemSlot* find_system(uint64_t handle) {
    if (kind_of(handle) != KIND_SYSTEM || slot_of(handle) >= MAX_SYSTEMS)
        return NULL;
    SystemSlot* s = &systems[slot_of(handle)];
    return s->generation && s->generation == generation_of(handle) ? s : NULL;
}
static RackSlot* find_rack(uint64_t handle) {
    if (kind_of(handle) != KIND_RACK || slot_of(handle) >= MAX_RACKS)
        return NULL;
    RackSlot* r = &racks[slot_of(handle)];
    return r->generation && r->generation == generation_of(handle) ? r : NULL;
}
/* A voice lives exactly as long as its rack (and so its system). */
static RackSlot* find_voice_rack(uint64_t handle) {
    if (kind_of(handle) != KIND_VOICE)
        return NULL;
    uint32_t rack_slot = slot_of(handle);
    if (rack_slot >= MAX_RACKS)
        return NULL;
    RackSlot* r = &racks[rack_slot];
    if (!r->generation || r->generation != generation_of(handle) || extra_of(handle) >= r->voices)
        return NULL;
    return r;
}

static void clear_buffer_info(ContextBufferInfo* info) {
    if (info)
        memset(info, 0, sizeof(*info));
}

static void announce_once(void) {
    if (announced)
        return;
    announced = 1;
    log_event("WARN", "muted_fallback_active reason=no_ngs2_module");
}

/* Reference SystemSetupCore: option fields, defaults when there is no option. */
static int32_t check_system_option(const SystemOption* option) {
    if (!option)
        return 0;
    if (option->size != SYSTEM_OPTION_SIZE)
        return NGS2_INVALID_OPTION_SIZE;
    uint32_t grain = option->max_grain_samples, num = option->num_grain_samples,
             rate = option->sample_rate;
    if (grain < 64 || grain > 1024 || (grain & 63))
        return NGS2_INVALID_MAX_GRAIN_SAMPLES;
    if (num < 64 || num > 1024 || (num & 63))
        return NGS2_INVALID_NUM_GRAIN_SAMPLES;
    static const uint32_t rates[] = {11025, 12000, 22050, 24000,  44100,
                                     48000, 88200, 96000, 176400, 192000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(*rates); ++i)
        if (rate == rates[i])
            return 0;
    return NGS2_INVALID_SAMPLE_RATE;
}

static ABI int32_t system_create_with_allocator(const SystemOption* option,
                                                const BufferAllocator* allocator, uint64_t* out) {
    if (!allocator || !allocator->alloc_handler)
        return NGS2_INVALID_BUFFER_ALLOCATOR;
    if (!out)
        return NGS2_INVALID_OUT_ADDRESS;
    int32_t result = check_system_option(option);
    if (result)
        return result;
    pthread_mutex_lock(&lock);
    result = NGS2_FAIL;
    for (uint32_t slot = 0; slot < MAX_SYSTEMS; ++slot) {
        if (systems[slot].generation)
            continue;
        systems[slot].generation = new_generation();
        *out = encode(KIND_SYSTEM, systems[slot].generation, slot, 0);
        result = 0;
        break;
    }
    pthread_mutex_unlock(&lock);
    if (!result)
        announce_once();
    return result;
}

static void destroy_rack_locked(uint32_t slot) {
    memset(&racks[slot], 0, sizeof(racks[slot]));
}

static ABI int32_t system_destroy(uint64_t handle, ContextBufferInfo* out_info) {
    pthread_mutex_lock(&lock);
    SystemSlot* system = find_system(handle);
    if (system) {
        for (uint32_t i = 0; i < MAX_RACKS; ++i)
            if (racks[i].generation && racks[i].system_slot == slot_of(handle) &&
                racks[i].system_generation == system->generation)
                destroy_rack_locked(i);
        system->generation = 0;
    }
    pthread_mutex_unlock(&lock);
    if (!system)
        return NGS2_INVALID_SYSTEM_HANDLE;
    clear_buffer_info(out_info);
    return 0;
}

static ABI int32_t system_render(uint64_t handle, const RenderBufferInfo* infos, uint32_t count) {
    pthread_mutex_lock(&lock);
    int valid = find_system(handle) != NULL;
    pthread_mutex_unlock(&lock);
    if (!valid)
        return NGS2_INVALID_SYSTEM_HANDLE;
    if (count && !infos)
        return NGS2_INVALID_BUFFER_ADDRESS;
    for (uint32_t i = 0; i < count; ++i)
        if (infos[i].buffer && infos[i].buffer_size)
            memset(infos[i].buffer, 0, infos[i].buffer_size);
    return 0;
}

static int32_t create_rack(uint64_t system_handle, const RackOption* option, uint64_t* out) {
    if (!out)
        return NGS2_INVALID_OUT_ADDRESS;
    if (option && option->size < sizeof(RackOption))
        return NGS2_INVALID_OPTION_SIZE;
    uint32_t voices = option && option->max_voices ? option->max_voices : MAX_VOICES;
    if (voices > MAX_VOICES)
        voices = MAX_VOICES;
    pthread_mutex_lock(&lock);
    SystemSlot* system = find_system(system_handle);
    int32_t result = system ? NGS2_FAIL : NGS2_INVALID_SYSTEM_HANDLE;
    for (uint32_t slot = 0; system && slot < MAX_RACKS; ++slot) {
        if (racks[slot].generation)
            continue;
        racks[slot].generation = new_generation();
        racks[slot].system_slot = slot_of(system_handle);
        racks[slot].system_generation = system->generation;
        racks[slot].voices = voices;
        *out = encode(KIND_RACK, racks[slot].generation, slot, 0);
        result = 0;
        break;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static ABI int32_t rack_create(uint64_t system, uint32_t rack_id, const RackOption* option,
                               const ContextBufferInfo* buffer, uint64_t* out) {
    (void)rack_id; /* [a verificar: the console rejects unknown rack ids (0x804A0260)] */
    if (!buffer)
        return NGS2_INVALID_BUFFER_INFO;
    return create_rack(system, option, out);
}

static ABI int32_t rack_create_with_allocator(uint64_t system, uint32_t rack_id,
                                              const RackOption* option,
                                              const BufferAllocator* allocator, uint64_t* out) {
    (void)rack_id;
    if (!allocator || !allocator->alloc_handler)
        return NGS2_INVALID_BUFFER_ALLOCATOR;
    return create_rack(system, option, out);
}

static ABI int32_t rack_destroy(uint64_t handle, ContextBufferInfo* out_info) {
    pthread_mutex_lock(&lock);
    RackSlot* rack = find_rack(handle);
    if (rack)
        destroy_rack_locked(slot_of(handle));
    pthread_mutex_unlock(&lock);
    if (!rack)
        return NGS2_INVALID_RACK_HANDLE;
    clear_buffer_info(out_info);
    return 0;
}

static ABI int32_t rack_get_voice_handle(uint64_t handle, uint32_t index, uint64_t* out) {
    pthread_mutex_lock(&lock);
    RackSlot* rack = find_rack(handle);
    int32_t result = rack ? 0 : NGS2_INVALID_RACK_HANDLE;
    if (rack && !out)
        result = NGS2_INVALID_OUT_ADDRESS;
    else if (rack && index >= rack->voices)
        result = NGS2_INVALID_VOICE_INDEX;
    else if (rack)
        *out = encode(KIND_VOICE, rack->generation, slot_of(handle), index);
    pthread_mutex_unlock(&lock);
    return result;
}

static ABI int32_t rack_query_buffer_size(uint32_t rack_id, const RackOption* option,
                                          ContextBufferInfo* out) {
    (void)rack_id;
    if (!out)
        return NGS2_INVALID_BUFFER_ADDRESS;
    if (option && option->size < sizeof(RackOption))
        return NGS2_INVALID_OPTION_SIZE;
    clear_buffer_info(out);
    out->host_buffer_size = RACK_BUFFER_BYTES;
    return 0;
}

static int voice_exists(uint64_t handle) {
    pthread_mutex_lock(&lock);
    int valid = find_voice_rack(handle) != NULL;
    pthread_mutex_unlock(&lock);
    return valid;
}

/* Parameter lists are walked by the console's module; with no DSP there is nothing to apply. */
static ABI int32_t voice_control(uint64_t handle, const void* params) {
    if (!voice_exists(handle))
        return NGS2_INVALID_VOICE_HANDLE;
    return params ? 0 : NGS2_INVALID_BUFFER_ADDRESS;
}

static ABI int32_t voice_get_state(uint64_t handle, void* out, uint64_t size) {
    if (!voice_exists(handle))
        return NGS2_INVALID_VOICE_HANDLE;
    if (!out)
        return NGS2_INVALID_OUT_ADDRESS;
    memset(out, 0, size < MAX_STATE_BYTES ? size : MAX_STATE_BYTES);
    return 0;
}

/* 0: no flag set, the voice is stopped. [a verificar: the flag values of the console] */
static ABI int32_t voice_get_state_flags(uint64_t handle, uint32_t* out) {
    if (!voice_exists(handle))
        return NGS2_INVALID_VOICE_HANDLE;
    if (!out)
        return NGS2_INVALID_OUT_ADDRESS;
    *out = 0;
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceNgs2SystemCreateWithAllocator", system_create_with_allocator},
    {"sceNgs2SystemDestroy", system_destroy},
    {"sceNgs2SystemRender", system_render},
    {"sceNgs2RackCreate", rack_create},
    {"sceNgs2RackCreateWithAllocator", rack_create_with_allocator},
    {"sceNgs2RackDestroy", rack_destroy},
    {"sceNgs2RackGetVoiceHandle", rack_get_voice_handle},
    {"sceNgs2RackQueryBufferSize", rack_query_buffer_size},
    {"sceNgs2VoiceControl", voice_control},
    {"sceNgs2VoiceGetState", voice_get_state},
    {"sceNgs2VoiceGetStateFlags", voice_get_state_flags},
};

uintptr_t runtime_ngs2_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
