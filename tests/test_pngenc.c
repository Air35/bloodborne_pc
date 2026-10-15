// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libScePngEnc (runtime_pngenc.c, plan docs/plans/2026-10-15-a3-hle-lote5.md). The PNG
 * produced is decoded back here (chunks, CRC-32, stored deflate blocks, Adler-32, pixels) with no
 * library. Layouts and error codes: shadPS4 b4e7ae7 libpng/pngenc.h, pngenc_error.h (consulted);
 * the arguments mirror the SotC call at eboot+0x963ab..0x963e7 (RGB, 8 bits, filter 0xf, level 6,
 * output buffer as large as the RGBA input). */
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
    uint32_t this_size, attribute, max_image_width, max_filter_number;
} CreateParam;
typedef struct {
    const uint8_t* image;
    uint8_t* png;
    uint32_t image_size, png_size, width, height, pitch;
    uint16_t pixel_format, color_space, bit_depth, clut_number, filter_type, compression_level;
} EncodeParam;
typedef struct {
    uint32_t data_size, processed_height;
} OutputInfo;
typedef int32_t(ABI* Query)(const CreateParam*);
typedef int32_t(ABI* Create)(const CreateParam*, void*, uint32_t, void**);
typedef int32_t(ABI* Encode)(void*, const EncodeParam*, OutputInfo*);
typedef int32_t(ABI* Delete)(void*);

enum { RGB = 3, RGBA = 19, R8G8B8A8 = 0, B8G8R8A8 = 1 };
#define INVALID_ADDR ((int32_t)0x80690101)
#define INVALID_SIZE ((int32_t)0x80690102)
#define INVALID_PARAM ((int32_t)0x80690103)
#define INVALID_HANDLE ((int32_t)0x80690104)
#define DATA_OVERFLOW ((int32_t)0x80690110)

static Query query;
static Create create;
static Encode encode;
static Delete destroy;

static void* resolve(const char* name) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#libScePngEnc", nid);
    return (void*)runtime_resolve(key, 0);
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t crc32_of(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

/* Inflates a zlib stream made only of stored blocks; returns its length or 0 on any error. */
static size_t inflate_stored(const uint8_t* z, size_t n, uint8_t* out, size_t cap) {
    if (n < 6 || z[0] != 0x78 || (z[0] << 8 | z[1]) % 31)
        return 0;
    size_t at = 2, len = 0;
    for (int final = 0; !final;) {
        if (at + 5 > n || (z[at] & 6))
            return 0; /* BTYPE must be 00 */
        final = z[at] & 1;
        const uint32_t block = (uint32_t)z[at + 1] | (uint32_t)z[at + 2] << 8;
        const uint32_t check = (uint32_t)z[at + 3] | (uint32_t)z[at + 4] << 8;
        at += 5;
        if ((block ^ 0xffffu) != check || at + block > n || len + block > cap)
            return 0;
        memcpy(out + len, z + at, block);
        at += block;
        len += block;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + out[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return at + 4 == n && be32(z + at) == (b << 16 | a) ? len : 0;
}

/* Checks the PNG and compares its pixels with the RGBA source; returns 1 when all match. */
static int decode_matches(const uint8_t* png, size_t size, const uint8_t* rgba, uint32_t w,
                          uint32_t h, int alpha, int bgr) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    static uint8_t zlib[1 << 20], raw[1 << 20];
    size_t at = 8, zlen = 0;
    int ended = 0;
    if (size < 8 || memcmp(png, signature, 8))
        return 0;
    while (!ended && at + 12 <= size) {
        const uint32_t len = be32(png + at);
        const uint8_t* type = png + at + 4;
        if (at + 12 + len > size || crc32_of(type, 4 + len) != be32(type + 4 + len))
            return 0;
        if (!memcmp(type, "IHDR", 4)) {
            const uint8_t* d = type + 4;
            if (len != 13 || be32(d) != w || be32(d + 4) != h || d[8] != 8 ||
                d[9] != (alpha ? 6 : 2) || d[10] || d[11] || d[12])
                return 0;
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(zlib + zlen, type + 4, len);
            zlen += len;
        } else if (!memcmp(type, "IEND", 4)) {
            ended = 1;
        }
        at += 12 + len;
    }
    const size_t channels = alpha ? 4 : 3, row = 1 + channels * w;
    if (!ended || at != size || inflate_stored(zlib, zlen, raw, sizeof(raw)) != row * h)
        return 0;
    for (uint32_t y = 0; y < h; ++y) {
        if (raw[y * row])
            return 0; /* filter type 0 */
        for (uint32_t x = 0; x < w; ++x)
            for (size_t c = 0; c < channels; ++c) {
                const size_t from = bgr && c < 3 ? 2 - c : c;
                if (raw[y * row + 1 + x * channels + c] != rgba[(y * w + x) * 4 + from])
                    return 0;
            }
    }
    return 1;
}

static void errors(void* handle, EncodeParam p) {
    CreateParam bad = {sizeof(CreateParam), 1, 64, 0};
    assert(query(NULL) == INVALID_ADDR && query(&bad) == INVALID_PARAM);
    bad = (CreateParam){sizeof(CreateParam), 0, 64, 6};
    assert(query(&bad) == INVALID_PARAM); /* max_filter_number > 5 */
    bad.max_filter_number = 0;
    bad.max_image_width = 0;
    assert(query(&bad) == INVALID_SIZE);
    assert(encode(NULL, &p, NULL) == INVALID_HANDLE && encode(handle, NULL, NULL) == INVALID_PARAM);
    EncodeParam q = p;
    q.png = NULL;
    assert(encode(handle, &q, NULL) == INVALID_ADDR);
    q = p;
    q.height = 0;
    assert(encode(handle, &q, NULL) == INVALID_SIZE);
    q = p;
    q.png_size = 40; /* too small for the stored stream */
    assert(encode(handle, &q, NULL) == DATA_OVERFLOW);
    /* Own safety checks (no reference code): never read past the image, only 8-bit RGB/RGBA. */
    q = p;
    q.image_size = p.pitch * (p.height - 1) + p.width * 4 - 1;
    assert(encode(handle, &q, NULL) == INVALID_SIZE);
    q = p;
    q.bit_depth = 16;
    assert(encode(handle, &q, NULL) == INVALID_PARAM);
    q = p;
    q.color_space = 7;
    assert(encode(handle, &q, NULL) == INVALID_PARAM);
}

int main(void) {
    runtime_start(1);
    query = (Query)resolve("scePngEncQueryMemorySize");
    create = (Create)resolve("scePngEncCreate");
    encode = (Encode)resolve("scePngEncEncode");
    destroy = (Delete)resolve("scePngEncDelete");
    assert(query && create && encode && destroy);

    /* 200 x 120: the RGB stream (120 * 601 bytes) needs two stored blocks. */
    enum { W = 200, H = 120 };
    static uint8_t rgba[W * H * 4], png[W * H * 5];
    for (size_t i = 0; i < sizeof(rgba); ++i)
        rgba[i] = (uint8_t)(i * 7 + i / 977);
    const CreateParam cp = {sizeof(CreateParam), 0, W, 0};
    const int32_t memory_size = query(&cp);
    assert(memory_size > 0 && memory_size <= 256);
    uint32_t memory[64]; /* 4-byte aligned, as malloc'ed guest memory would be */
    void* handle = NULL;
    assert(create(&cp, NULL, (uint32_t)memory_size, &handle) == INVALID_ADDR);
    assert(create(&cp, memory, (uint32_t)memory_size - 1, &handle) == INVALID_SIZE);
    assert(create(&cp, memory, (uint32_t)memory_size, &handle) == 0 && handle);

    /* The SotC call (eboot+0x963ab..0x963e7): RGB, 8 bits, filter 0xf, level 6, no output info,
     * and the same value for image size and PNG buffer size (one register; that it equals the
     * RGBA size is [a verificar]); here the synthetic RGBA size. */
    EncodeParam p = {rgba, png, sizeof(rgba), sizeof(rgba), W, H, W * 4, R8G8B8A8, RGB, 8, 0,
                     0xf,  6};
    const int32_t rgb_size = encode(handle, &p, NULL);
    assert(rgb_size > 0 && decode_matches(png, (size_t)rgb_size, rgba, W, H, 0, 0));

    /* RGBA stored blocks do not fit a buffer of the RGBA image size: overflow, nothing written. */
    memset(png, 0xEE, 64);
    p.color_space = RGBA;
    assert(encode(handle, &p, NULL) == DATA_OVERFLOW && png[0] == 0xEE);

    OutputInfo info = {0, 0};
    p.png_size = sizeof(png);
    p.pixel_format = B8G8R8A8;
    const int32_t rgba_size = encode(handle, &p, &info);
    assert(rgba_size > 0 && info.data_size == (uint32_t)rgba_size && info.processed_height == H);
    assert(decode_matches(png, (size_t)rgba_size, rgba, W, H, 1, 1));

    p.color_space = RGB;
    p.pixel_format = R8G8B8A8;
    errors(handle, p);
    assert(destroy(handle) == 0 && encode(handle, &p, NULL) == INVALID_HANDLE);
    puts("PASS: PNG encoder (stored deflate, RGB/RGBA, BGR swap, chunk CRCs, errors)");
    return 0;
}
