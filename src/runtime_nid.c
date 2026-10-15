// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: NID of a PS4 symbol name, so the runtime's by-name tables also serve imports the
 * original game (Bloodborne) never had in import_names.inc. NID = base64 of the first 8 bytes
 * of SHA-1(name + fixed salt), byte-reversed, '/' -> '-', no padding: the same computation as
 * scripts/prepare.py:106-109 (nid). Plan: docs/plans/2026-10-14-a1-imports-nid-biblioteca.md. */
#include <stdint.h>
#include <string.h>

#include "runtime.h"

static uint32_t rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* One 64-byte SHA-1 block into h[5] (FIPS 180-4 §6.1.2). */
static void sha1_block(uint32_t h[5], const unsigned char* p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

/* SHA-1 of a message up to 256 bytes (symbol name + 16-byte salt); digest in out[20]. */
static int sha1_short(const unsigned char* msg, size_t len, unsigned char out[20]) {
    unsigned char buf[320] = {0};
    if (len > 256)
        return 0;
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    const size_t total = (len + 9 + 63) / 64 * 64;
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        buf[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (size_t off = 0; off < total; off += 64)
        sha1_block(h, buf + off);
    for (int i = 0; i < 20; ++i)
        out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
    return 1;
}

int runtime_nid(const char* name, char out[12]) {
    static const unsigned char salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                           0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    unsigned char msg[256], digest[20], v[9] = {0};
    const size_t len = strlen(name);
    if (len > sizeof(msg) - sizeof(salt)) {
        out[0] = 0;
        return 0;
    }
    memcpy(msg, name, len);
    memcpy(msg + len, salt, sizeof(salt));
    if (!sha1_short(msg, len + sizeof(salt), digest)) {
        out[0] = 0;
        return 0;
    }
    for (int i = 0; i < 8; ++i)
        v[i] = digest[7 - i]; /* first 8 bytes, reversed */
    /* base64 of 8 bytes = 11 symbols (the last one carries 4 bits: v[8] stays 0). */
    for (int i = 0, o = 0; i < 9; i += 3) {
        const uint32_t n = (uint32_t)v[i] << 16 | (uint32_t)v[i + 1] << 8 | v[i + 2];
        for (int j = 0; j < 4 && o < 11; ++j)
            out[o++] = alphabet[(n >> (18 - 6 * j)) & 63];
    }
    out[11] = 0;
    return 1;
}
