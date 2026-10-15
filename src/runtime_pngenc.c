// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: libScePngEnc (docs/plans/2026-10-15-a3-hle-lote5.md). The SotC encodes RGB 8-bit images
 * (call at eboot+0x963ab..0x963e7). Contract consulted in shadPS4 b4e7ae7 libpng/pngenc.{h,cpp}
 * and pngenc_error.h (it uses libpng; nothing copied): Create keeps its state in the memory the
 * guest provides, QueryMemorySize returns its size, Encode returns the PNG size in bytes or a
 * negative error. The encoder here is our own and dependency free: deflate "stored" blocks (no
 * compression), CRC-32 and Adler-32 per RFC 1950/1951 and the PNG spec. The file is valid but
 * larger than the console's; filter type and compression level are accepted and ignored.
 * Size budget: the game passes the same value as image size and PNG buffer size (one register,
 * eboot+0x963bc/0x963c3); that it is width*height*4 is [a verificar]. If it is, the RGB stream
 * (3/4 of it plus ~5 bytes per 64 KiB) fits; an RGBA stream would not and answers DATA_OVERFLOW,
 * which is the path the game already has for a too-small buffer. */
#define _GNU_SOURCE
#include <string.h>

#include "runtime.h"

#define PNG_ERROR(n) ((int32_t)(UINT32_C(0x80690000) | (n))) /* pngenc_error.h */
enum {
    PNG_INVALID_ADDR = 0x101,
    PNG_INVALID_SIZE = 0x102,
    PNG_INVALID_PARAM = 0x103,
    PNG_INVALID_HANDLE = 0x104,
    PNG_DATA_OVERFLOW = 0x110,
};
enum { COLOR_RGB = 3, COLOR_RGBA = 19, FORMAT_RGBA = 0, FORMAT_BGRA = 1 };
enum { MAX_WIDTH = 1000000, MAX_FILTERS = 5, STORED_MAX = 65535, ROW_PIXELS = 256 };

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

/* State kept in the guest's memory block; the magic makes a deleted handle detectable. The
 * creation width is not kept: the reference ignores it when encoding. */
typedef struct {
    uint32_t magic;
} Handle;
#define HANDLE_MAGIC UINT32_C(0x474E5053) /* "SPNG" */

/* Byte sink over the guest's PNG buffer; `at` never passes `size` (capacity is checked first). */
typedef struct {
    uint8_t* base;
    size_t at;
    uint32_t adler_a, adler_b;
    size_t raw_left;   /* raw (unfiltered-stream) bytes still to emit */
    size_t block_left; /* bytes left in the current stored block */
} Sink;

static uint32_t crc32_of(const uint8_t* data, size_t size) {
    uint32_t table[256];
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void put_be32(Sink* s, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        s->base[s->at++] = (uint8_t)(v >> shift);
}

static void put_bytes(Sink* s, const void* data, size_t size) {
    memcpy(s->base + s->at, data, size);
    s->at += size;
}

/* chunk = length, type, data, CRC(type + data); `begin` is where the type starts. */
static size_t chunk_begin(Sink* s, const char type[4], uint32_t length) {
    put_be32(s, length);
    const size_t begin = s->at;
    put_bytes(s, type, 4);
    return begin;
}

static void chunk_end(Sink* s, size_t begin) {
    put_be32(s, crc32_of(s->base + begin, s->at - begin));
}

static size_t ceil_div(size_t a, size_t b) {
    return (a + b - 1) / b;
}

/* IDAT data length for a raw stream of `raw` bytes: zlib header, stored blocks with 5 bytes of
 * header each, Adler-32. */
static uint64_t idat_length(uint64_t raw) {
    return 2 + 5 * ceil_div(raw, STORED_MAX) + raw + 4;
}

/* Total bytes of the PNG: signature, IHDR chunk, IDAT chunk, IEND chunk (12 bytes of framing
 * per chunk: length, type, CRC). */
static uint64_t png_size_for(uint64_t raw) {
    return 8 + (12 + 13) + (12 + idat_length(raw)) + 12;
}

/* Starts the next stored block when the current one is full. */
static void block_open(Sink* s) {
    if (s->block_left)
        return;
    const size_t len = s->raw_left < STORED_MAX ? s->raw_left : STORED_MAX;
    s->base[s->at++] = len == s->raw_left; /* BFINAL, BTYPE = 00 */
    s->base[s->at++] = (uint8_t)len;
    s->base[s->at++] = (uint8_t)(len >> 8);
    s->base[s->at++] = (uint8_t)~len;
    s->base[s->at++] = (uint8_t)(~len >> 8);
    s->block_left = len;
}

/* Appends raw stream bytes, opening stored blocks as needed, and folds them into the Adler-32. */
static void emit(Sink* s, const uint8_t* data, size_t size) {
    while (size) {
        block_open(s);
        const size_t take = size < s->block_left ? size : s->block_left;
        memcpy(s->base + s->at, data, take);
        for (size_t i = 0; i < take; ++i) {
            s->adler_a = (s->adler_a + data[i]) % 65521u;
            s->adler_b = (s->adler_b + s->adler_a) % 65521u;
        }
        s->at += take;
        s->block_left -= take;
        s->raw_left -= take;
        data += take;
        size -= take;
    }
}

/* One scanline: filter byte 0, then the pixels as RGB or RGBA, swapping B/R for BGRA input. */
static void emit_row(Sink* s, const uint8_t* src, uint32_t width, int alpha, int bgr) {
    static const uint8_t no_filter = 0;
    uint8_t out[ROW_PIXELS * 4];
    emit(s, &no_filter, 1);
    for (uint32_t done = 0; done < width;) {
        const uint32_t count = width - done < ROW_PIXELS ? width - done : ROW_PIXELS;
        size_t n = 0;
        for (uint32_t x = 0; x < count; ++x, src += 4) {
            out[n++] = src[bgr ? 2 : 0];
            out[n++] = src[1];
            out[n++] = src[bgr ? 0 : 2];
            if (alpha)
                out[n++] = src[3];
        }
        emit(s, out, n);
        done += count;
    }
}

static void write_header(Sink* s, const EncodeParam* p, int alpha) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put_bytes(s, signature, sizeof(signature));
    const size_t begin = chunk_begin(s, "IHDR", 13);
    put_be32(s, p->width);
    put_be32(s, p->height);
    const uint8_t depth_color_methods[5] = {8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0};
    put_bytes(s, depth_color_methods, sizeof(depth_color_methods));
    chunk_end(s, begin);
}

static void write_image(Sink* s, const EncodeParam* p, int alpha, int bgr) {
    static const uint8_t zlib_header[2] = {0x78, 0x01};
    const size_t raw = (size_t)p->height * (1 + (alpha ? 4 : 3) * (size_t)p->width);
    const size_t begin = chunk_begin(s, "IDAT", (uint32_t)idat_length(raw));
    put_bytes(s, zlib_header, sizeof(zlib_header));
    s->raw_left = raw;
    s->adler_a = 1;
    s->adler_b = 0;
    for (uint32_t y = 0; y < p->height; ++y)
        emit_row(s, p->image + (size_t)y * p->pitch, p->width, alpha, bgr);
    put_be32(s, s->adler_b << 16 | s->adler_a);
    chunk_end(s, begin);
    const size_t end = chunk_begin(s, "IEND", 0);
    chunk_end(s, end);
}

/* Reference order for the first checks (pngenc.cpp scePngEncEncode); the rest are our own
 * safety checks for guest memory, which the reference leaves to libpng [a verificar: console
 * codes]. */
static int32_t encode_check(const Handle* handle, const EncodeParam* p) {
    if (!handle || handle->magic != HANDLE_MAGIC)
        return PNG_ERROR(PNG_INVALID_HANDLE);
    if (!p)
        return PNG_ERROR(PNG_INVALID_PARAM);
    if (!p->image || !p->png)
        return PNG_ERROR(PNG_INVALID_ADDR);
    if (!p->png_size || !p->image_size || !p->height || !p->width)
        return PNG_ERROR(PNG_INVALID_SIZE);
    if ((p->color_space != COLOR_RGB && p->color_space != COLOR_RGBA) || p->bit_depth != 8 ||
        (p->pixel_format != FORMAT_RGBA && p->pixel_format != FORMAT_BGRA))
        return PNG_ERROR(PNG_INVALID_PARAM);
    const uint64_t pitch = p->pitch ? p->pitch : (uint64_t)p->width * 4;
    if (p->width > MAX_WIDTH || pitch < (uint64_t)p->width * 4 ||
        pitch * (p->height - 1) + (uint64_t)p->width * 4 > p->image_size)
        return PNG_ERROR(PNG_INVALID_SIZE);
    return 0;
}

static ABI int32_t png_query_memory(const CreateParam* param) {
    if (!param)
        return PNG_ERROR(PNG_INVALID_ADDR);
    if (param->attribute || param->max_filter_number > MAX_FILTERS)
        return PNG_ERROR(PNG_INVALID_PARAM);
    if (param->max_image_width - 1 > MAX_WIDTH)
        return PNG_ERROR(PNG_INVALID_SIZE);
    return (int32_t)sizeof(Handle);
}

static ABI int32_t png_create(const CreateParam* param, void* memory, uint32_t size, Handle** out) {
    if (!param || param->attribute || !memory || !out)
        return PNG_ERROR(PNG_INVALID_ADDR);
    if (param->max_image_width - 1 > MAX_WIDTH || size < sizeof(Handle)) /* size: our check */
        return PNG_ERROR(PNG_INVALID_SIZE);
    Handle* handle = memory;
    handle->magic = HANDLE_MAGIC;
    *out = handle;
    return 0;
}

static ABI int32_t png_delete(Handle* handle) {
    if (!handle || handle->magic != HANDLE_MAGIC)
        return PNG_ERROR(PNG_INVALID_HANDLE);
    handle->magic = 0;
    return 0;
}

static ABI int32_t png_encode(Handle* handle, const EncodeParam* p, OutputInfo* info) {
    const int32_t error = encode_check(handle, p);
    if (error)
        return error;
    const int alpha = p->color_space == COLOR_RGBA;
    const size_t raw = (size_t)p->height * (1 + (alpha ? 4 : 3) * (size_t)p->width);
    const uint64_t total = png_size_for(raw);
    if (total > p->png_size || total > INT32_MAX)
        return PNG_ERROR(PNG_DATA_OVERFLOW); /* nothing written */
    Sink sink = {.base = p->png};
    EncodeParam local = *p;
    local.pitch = p->pitch ? p->pitch : p->width * 4;
    write_header(&sink, &local, alpha);
    write_image(&sink, &local, alpha, p->pixel_format == FORMAT_BGRA);
    if (info) {
        info->data_size = (uint32_t)sink.at;
        info->processed_height = p->height;
    }
    return (int32_t)sink.at;
}

static const RuntimeExport exports[] = {
    {"scePngEncQueryMemorySize", png_query_memory},
    {"scePngEncCreate", png_create},
    {"scePngEncDelete", png_delete},
    {"scePngEncEncode", png_encode},
};

uintptr_t runtime_pngenc_resolve(const char* name) {
    return RUNTIME_LOOKUP(exports, name);
}
