// SPDX-License-Identifier: GPL-2.0-or-later
/* See isa_rewrite.h. Adapted from gow3 91b6e61 src/isa_rewrite.c: SHA-256, stub emission, site
 * patching, trap lookup and report are the same design; this file adds the in-memory parser, the
 * stricter record validation (audit finding 3), the SSE4a XMM spill/reload and the ucontext
 * handler. */
#define _GNU_SOURCE
#include "isa_rewrite.h"

#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

enum { JMP_BYTES = 5 };

/* ---- SHA-256 (FIPS 180-4), for isa_table_matches ---- */

static uint32_t ror32(uint32_t x, int n) {
    return x >> n | x << (32 - n);
}

static void sha256_block(uint32_t h[8], const unsigned char* block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
               (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ w[i - 15] >> 3;
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = v[7] + (ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25)) +
                            ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
        const uint32_t t2 = (ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22)) +
                            ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        h[i] += v[i];
}

static void sha256(const unsigned char* data, uint64_t size, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t done = 0;
    for (; size - done >= 64; done += 64)
        sha256_block(h, data + done);
    unsigned char tail[128] = {0};
    const size_t rest = (size_t)(size - done), tail_size = rest < 56 ? 64 : 128;
    memcpy(tail, data + done, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++)
        tail[tail_size - 1 - i] = (unsigned char)(size * 8 >> 8 * i);
    for (size_t at = 0; at < tail_size; at += 64)
        sha256_block(h, tail + at);
    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- table ---- */

static uint64_t read_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static uint32_t read_u32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int register_valid(uint8_t reg) {
    return reg <= 15 || reg == ISA_NO_REG;
}

static int is_sse4a(uint8_t kind) {
    return kind == ISA_EXTRQ || kind == ISA_INSERTQ;
}

/* Operand rules of the SSE4a kinds: XMM registers only, no memory operand, INSERTQ always has a
 * source register, EXTRQ has one unless its length/index are immediates. */
static int sse4a_valid(const IsaSite* s) {
    if (s->mem_slot != ISA_MEM_NONE || (s->mem_flags & ~ISA_FLAG_IMM) != 0 || s->dst > 15)
        return 0;
    if (s->kind == ISA_INSERTQ)
        return s->src1 <= 15;
    return (s->mem_flags & ISA_FLAG_IMM) ? s->src1 == ISA_NO_REG || s->src1 <= 15 : s->src1 <= 15;
}

/* Field ranges of the format, plus what the patch writes: a 5-byte jmp needs a 5-byte site (mode
 * 0) or a relocated instruction (mode 1), an int3 site is 4 bytes, every write stays inside the
 * image, and (audit finding 3) a RIP-relative operand stays inside it too. */
static int record_valid(const IsaSite* s, uint64_t image_size) {
    if (s->length < 4 || s->length > 15 || s->relocated_length > 15)
        return 0;
    if (s->kind < ISA_MOVBE_LOAD || s->kind > ISA_KIND_LAST || s->mode > ISA_MODE_UNSUPPORTED)
        return 0;
    if (!register_valid(s->dst) || !register_valid(s->src1) || !register_valid(s->src2) ||
        !register_valid(s->base) || !register_valid(s->index) || s->mem_slot > ISA_MEM_SRC)
        return 0;
    if (is_sse4a(s->kind)) {
        if (!sse4a_valid(s))
            return 0;
    } else {
        if ((s->mem_flags & ~ISA_MEM_RIP_RELATIVE) != 0)
            return 0;
        if (s->kind <= ISA_BLSR && s->width != 16 && s->width != 32 && s->width != 64)
            return 0;
    }
    if (s->scale != 1 && s->scale != 2 && s->scale != 4 && s->scale != 8 &&
        !(s->scale == 0 && s->index == ISA_NO_REG))
        return 0;
    if (s->mem_flags & ISA_MEM_RIP_RELATIVE) {
        const uint64_t access = s->kind <= ISA_BLSR ? (uint64_t)s->width / 8 : 8;
        if (s->disp < 0 || (uint64_t)s->disp > image_size ||
            access > image_size - (uint64_t)s->disp)
            return 0;
    }
    if (s->mode == ISA_MODE_JMP && s->length < JMP_BYTES)
        return 0;
    if (s->mode == ISA_MODE_INT3 && s->length != 4)
        return 0;
    if (s->mode == ISA_MODE_JMP_RELOCATE && (s->length != 4 || s->relocated_length == 0))
        return 0;
    const uint64_t end = s->mode == ISA_MODE_JMP_RELOCATE ? 4u + s->relocated_length : s->length;
    return s->offset <= image_size && end <= image_size - s->offset;
}

static IsaTable* parse_fail(IsaTable* t, const char* reason, unsigned index) {
    if (index == (unsigned)-1)
        fprintf(stderr, "ISA: %s\n", reason);
    else
        fprintf(stderr, "ISA: %s %u\n", reason, index);
    isa_table_free(t);
    return NULL;
}

IsaTable* isa_table_parse(const unsigned char* data, size_t size) {
    if (size < ISA_TABLE_HEADER_BYTES || memcmp(data, ISA_TABLE_MAGIC, 8) != 0)
        return parse_fail(NULL, "not an ISATAB1 table", (unsigned)-1);
    const uint32_t count = read_u32(data + 48);
    if (count > ISA_TABLE_MAX_SITES)
        return parse_fail(NULL, "table has too many sites", (unsigned)-1);
    if (size != ISA_TABLE_HEADER_BYTES + (size_t)ISA_TABLE_RECORD_BYTES * count)
        return parse_fail(NULL, "table size does not match its site count", (unsigned)-1);
    IsaTable* t = calloc(1, sizeof *t);
    if (!t)
        return parse_fail(NULL, "out of memory", (unsigned)-1);
    t->sites = calloc(count ? count : 1, sizeof *t->sites);
    t->hits = calloc(count ? count : 1, sizeof *t->hits);
    if (!t->sites || !t->hits)
        return parse_fail(t, "out of memory", (unsigned)-1);
    memcpy(t->sites, data + ISA_TABLE_HEADER_BYTES, (size_t)count * sizeof *t->sites);
    t->count = count;
    t->unsupported = read_u32(data + 52);
    t->flags = read_u32(data + 56);
    t->image_size = read_u64(data + 8);
    memcpy(t->sha256, data + 16, sizeof t->sha256);
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && t->sites[i].offset <= t->sites[i - 1].offset)
            return parse_fail(t, "table records are not sorted at record", i);
        if (!record_valid(&t->sites[i], t->image_size))
            return parse_fail(t, "invalid table record", i);
    }
    return t;
}

IsaTable* isa_table_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ISA: cannot open rewrite table %s\n", path);
        return NULL;
    }
    unsigned char* data = NULL;
    size_t size = 0, capacity = 0;
    for (;;) {
        if (size == capacity) {
            capacity = capacity ? capacity * 2 : 1u << 16;
            unsigned char* grown = realloc(data, capacity);
            if (!grown) {
                free(data);
                fclose(f);
                fprintf(stderr, "ISA: out of memory\n");
                return NULL;
            }
            data = grown;
        }
        const size_t n = fread(data + size, 1, capacity - size, f);
        size += n;
        if (n == 0)
            break;
    }
    const int failed = ferror(f);
    fclose(f);
    IsaTable* t = failed ? NULL : isa_table_parse(data, size);
    if (failed)
        fprintf(stderr, "ISA: cannot read table %s\n", path);
    free(data);
    return t;
}

void isa_table_free(IsaTable* t) {
    if (!t)
        return;
    free(t->sites);
    free(t->hits);
    free(t);
}

int isa_table_matches(const IsaTable* t, const unsigned char* image, uint64_t size) {
    if (size != t->image_size)
        return 1;
    uint8_t digest[32];
    sha256(image, size, digest);
    return memcmp(digest, t->sha256, sizeof digest) != 0;
}

size_t isa_stub_area_size(const IsaTable* t) {
    return (size_t)t->count * ISA_STUB_BYTES;
}

unsigned isa_site_set(const IsaSite* s) {
    switch (s->kind) {
    case ISA_MOVBE_LOAD:
    case ISA_MOVBE_STORE:
        return CPU_SET_MOVBE;
    case ISA_LZCNT:
        return CPU_SET_LZCNT;
    case ISA_TZCNT:
    case ISA_ANDN:
    case ISA_BEXTR:
    case ISA_BLSI:
    case ISA_BLSMSK:
    case ISA_BLSR:
        return CPU_SET_BMI1;
    case ISA_VCVTPH2PS:
    case ISA_VCVTPS2PH:
        return CPU_SET_F16C;
    case ISA_EXTRQ:
    case ISA_INSERTQ:
        return CPU_SET_SSE4A;
    default:
        return 0;
    }
}

/* ---- stub emission ---- */

static void emit_u8(unsigned char** p, uint8_t v) {
    *(*p)++ = v;
}
static void emit_u32(unsigned char** p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        emit_u8(p, (uint8_t)(v >> 8 * i));
}
static void emit_u64(unsigned char** p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        emit_u8(p, (uint8_t)(v >> 8 * i));
}
static void emit_bytes(unsigned char** p, const uint8_t* bytes, size_t n) {
    memcpy(*p, bytes, n);
    *p += n;
}

/* mov [rsp+disp8], reg (opcode 89) or mov reg, [rsp+disp8] (8B): REX.W(.R) modrm(01 reg 100)
 * SIB 24. */
static void emit_rsp_mov(unsigned char** p, uint8_t opcode, int reg, uint8_t disp) {
    emit_u8(p, reg >= 8 ? 0x4c : 0x48);
    emit_u8(p, opcode);
    emit_u8(p, (uint8_t)(0x44 | (reg & 7) << 3));
    emit_u8(p, 0x24);
    emit_u8(p, disp);
}

/* movups [rsp+disp32], xmm (0F 11) or movlps xmm, [rsp+disp32] (0F 12): [REX.R] 0F op
 * modrm(10 reg 100) SIB 24 disp32. Legacy SSE, as the SSE4a instruction it stands in for. */
static void emit_xmm_rsp(unsigned char** p, uint8_t opcode, int reg, uint32_t disp) {
    if (reg >= 8)
        emit_u8(p, 0x44);
    emit_u8(p, 0x0f);
    emit_u8(p, opcode);
    emit_u8(p, (uint8_t)(0x84 | (reg & 7) << 3));
    emit_u8(p, 0x24);
    emit_u32(p, disp);
}

/* rel32 from the byte after a jmp ending at `next` to `target`; 0 when it does not fit. */
static int rel32_to(const unsigned char* next, const unsigned char* target, int32_t* rel) {
    const int64_t delta = (int64_t)((intptr_t)target - (intptr_t)next);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return 0;
    *rel = (int32_t)delta;
    return 1;
}

/* Frame offsets, from rsp right after `lea rsp,[rsp-0x180]`: gpr[i] at 8*i; the XMM images of
 * IsaRegs.xmm follow the gprs (IsaRegs starts 8 bytes below, at the pushed rflags). The frame is
 * 0x80 bytes larger than what it holds so that nothing lands in the guest's red zone (the 128
 * bytes below its rsp, SysV ABI): with 0x100 the XMM images overwrote [rsp-0x80, rsp-0x60) of a
 * leaf function (tests/test_isa_rewrite.c, test_stub_keeps_red_zone). */
enum { FRAME_BYTES = 0x180, XMM_DST_AT = 0x80, XMM_SRC_AT = 0x90 };
_Static_assert(FRAME_BYTES - (XMM_SRC_AT + 16) >= 128, "the stub frame must stay below the red zone");

static void emit_prologue(unsigned char** p, const IsaSite* s) {
    static const uint8_t lea_rsp_down[] = {0x48, 0x8d, 0xa4, 0x24,
                                           0x80, 0xfe, 0xff, 0xff}; /* lea rsp,[rsp-0x180] */
    static const uint8_t lea_rax_rsp_up[] = {0x48, 0x8d, 0x84, 0x24,
                                             0x80, 0x01, 0x00, 0x00}; /* lea rax,[rsp+0x180] */
    _Static_assert(FRAME_BYTES == 0x180, "the lea displacements above encode FRAME_BYTES");
    emit_bytes(p, lea_rsp_down, sizeof lea_rsp_down);
    for (int reg = 0; reg < 16; reg++)
        if (reg != 4)
            emit_rsp_mov(p, 0x89, reg, (uint8_t)(8 * reg));
    emit_bytes(p, lea_rax_rsp_up, sizeof lea_rax_rsp_up);
    emit_rsp_mov(p, 0x89, 0, 8 * 4); /* gpr[4] = guest rsp */
    if (is_sse4a(s->kind)) {
        emit_xmm_rsp(p, 0x11, s->dst, XMM_DST_AT);
        if (s->src1 != ISA_NO_REG)
            emit_xmm_rsp(p, 0x11, s->src1, XMM_SRC_AT);
    }
}

static void emit_call(unsigned char** p, const IsaSite* s, uint64_t* hit) {
    emit_u8(p, 0x9c);                                /* pushfq: rsp = IsaRegs* */
    emit_bytes(p, (const uint8_t[]){0x48, 0xb8}, 2); /* mov rax, &hits[i] */
    emit_u64(p, (uint64_t)(uintptr_t)hit);
    emit_bytes(p, (const uint8_t[]){0x48, 0xff, 0x00}, 3);       /* inc qword [rax] */
    emit_bytes(p, (const uint8_t[]){0x48, 0x89, 0xe5}, 3);       /* mov rbp, rsp */
    emit_bytes(p, (const uint8_t[]){0x48, 0x83, 0xe4, 0xf0}, 4); /* and rsp, -16 */
    emit_bytes(p, (const uint8_t[]){0x48, 0xbf}, 2);             /* mov rdi, site */
    emit_u64(p, (uint64_t)(uintptr_t)s);
    emit_bytes(p, (const uint8_t[]){0x48, 0x89, 0xee}, 3); /* mov rsi, rbp */
    emit_bytes(p, (const uint8_t[]){0x48, 0xb8}, 2);       /* mov rax, isa_emulate */
    emit_u64(p, (uint64_t)(uintptr_t)isa_emulate);
    emit_bytes(p, (const uint8_t[]){0xff, 0xd0}, 2);       /* call rax */
    emit_bytes(p, (const uint8_t[]){0x48, 0x89, 0xec}, 3); /* mov rsp, rbp */
    emit_u8(p, 0x9d);                                      /* popfq: rsp = &gpr[0] */
}

static void emit_epilogue(unsigned char** p, const IsaSite* s) {
    static const uint8_t lea_rsp_up[] = {0x48, 0x8d, 0xa4, 0x24,
                                         0x80, 0x01, 0x00, 0x00}; /* lea rsp,[rsp+0x180] */
    for (int reg = 0; reg < 16; reg++)
        if (reg != 4)
            emit_rsp_mov(p, 0x8b, reg, (uint8_t)(8 * reg));
    if (is_sse4a(s->kind))
        emit_xmm_rsp(p, 0x12, s->dst, XMM_DST_AT); /* movlps: only the low quadword changes */
    emit_bytes(p, lea_rsp_up, sizeof lea_rsp_up);
}

/* Writes the stub for `s` into `code` (laid out to run at `at`). Returns its size, or 0 when the
 * jmp back is out of rel32 range. */
static size_t emit_stub(unsigned char* code, const unsigned char* at, const IsaSite* s,
                        uint64_t* hit, unsigned char* image) {
    unsigned char* p = code;
    emit_prologue(&p, s);
    emit_call(&p, s, hit);
    emit_epilogue(&p, s);
    const unsigned char* back = image + s->offset + s->length;
    if (s->mode == ISA_MODE_JMP_RELOCATE) {
        emit_bytes(&p, s->relocated, s->relocated_length);
        back = image + s->offset + 4 + s->relocated_length;
    }
    int32_t rel;
    if (!rel32_to(at + (p - code) + JMP_BYTES, back, &rel))
        return 0;
    emit_u8(&p, 0xe9);
    emit_u32(&p, (uint32_t)rel);
    return (size_t)(p - code);
}

/* The bytes written at a site: jmp rel32 + nops over the rest it covers, or a single int3. */
static void patch_site(unsigned char* site, const IsaSite* s, int32_t rel) {
    if (s->mode == ISA_MODE_INT3) {
        site[0] = 0xcc;
        return;
    }
    unsigned char* p = site;
    emit_u8(&p, 0xe9);
    emit_u32(&p, (uint32_t)rel);
    const size_t covered = s->mode == ISA_MODE_JMP_RELOCATE ? 4u + s->relocated_length : s->length;
    memset(p, 0x90, covered - JMP_BYTES);
}

/* Applies one site; adds to the report. */
static void apply_site(IsaTable* t, uint32_t i, unsigned char* image, unsigned char* stubs,
                       IsaReport* report) {
    const IsaSite* s = &t->sites[i];
    unsigned char* site = image + s->offset;
    if (memcmp(site, s->original, s->length) != 0) {
        fprintf(stderr, "ISA: site 0x%llx %s skipped: bytes changed\n",
                (unsigned long long)s->offset, isa_kind_name(s->kind));
        report->skipped++;
        return;
    }
    if (s->mode == ISA_MODE_JMP_RELOCATE &&
        memcmp(site + 4, s->relocated, s->relocated_length) != 0) {
        /* A patch or hook changed the instruction the jmp would cover: the stub would run the old
         * copy. */
        fprintf(stderr, "ISA: site 0x%llx %s: next instruction changed, int3 fallback\n",
                (unsigned long long)s->offset, isa_kind_name(s->kind));
        t->sites[i].mode = ISA_MODE_INT3;
    }
    if (s->mode == ISA_MODE_INT3) {
        patch_site(site, s, 0);
        report->int3++;
        report->rewritten++;
        return;
    }
    unsigned char* stub = stubs + (size_t)i * ISA_STUB_BYTES;
    unsigned char code[2 * ISA_STUB_BYTES];
    int32_t rel;
    size_t size;
    if (!rel32_to(site + JMP_BYTES, stub, &rel) ||
        (size = emit_stub(code, stub, s, &t->hits[i], image)) == 0) {
        fprintf(stderr, "ISA: site 0x%llx out of rel32 range\n", (unsigned long long)s->offset);
        report->skipped++;
        return;
    }
    if (size > ISA_STUB_BYTES) {
        fprintf(stderr, "ISA: stub for site 0x%llx exceeds %d bytes, site skipped\n",
                (unsigned long long)s->offset, (int)ISA_STUB_BYTES);
        report->skipped++;
        return;
    }
    memcpy(stub, code, size);
    patch_site(site, s, rel);
    if (s->mode == ISA_MODE_JMP)
        report->jmp++;
    else
        report->relocated++;
    report->rewritten++;
}

IsaReport isa_rewrite_apply(IsaTable* t, unsigned char* image, unsigned char* stubs,
                            unsigned sets) {
    IsaReport report = {0};
    t->image = image;
    t->stubs = stubs;
    isa_emulate_set_image(image);
    for (uint32_t i = 0; i < t->count; i++)
        if (t->sites[i].mode == ISA_MODE_UNSUPPORTED && (isa_site_set(&t->sites[i]) & sets))
            report.unsupported_needed++;
    if (report.unsupported_needed)
        return report; /* writes nothing */
    for (uint32_t i = 0; i < t->count; i++) {
        const IsaSite* s = &t->sites[i];
        if (s->mode == ISA_MODE_UNSUPPORTED || !(isa_site_set(s) & sets))
            continue;
        apply_site(t, i, image, stubs, &report);
    }
    return report;
}

/* ---- int3 trap, stub lookup, report ---- */

/* Index of the INT3 site at `address` in the image, or -1. */
static int int3_site_at(const IsaTable* t, uintptr_t address) {
    if (!t || !t->image || address < (uintptr_t)t->image)
        return -1;
    const uint64_t offset = address - (uintptr_t)t->image;
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (t->sites[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == t->count || t->sites[lo].offset != offset)
        return -1;
    if (t->sites[lo].mode != ISA_MODE_INT3 || *(const unsigned char*)address != 0xcc)
        return -1;
    return (int)lo;
}

int isa_rewrite_trap(IsaTable* t, uintptr_t address, IsaRegs* regs, uintptr_t* next_rip) {
    const int site = int3_site_at(t, address);
    if (site < 0)
        return 0;
    isa_emulate(&t->sites[site], regs);
    t->hits[site]++;
    *next_rip = address + t->sites[site].length;
    return 1;
}

/* ucontext gregs of IsaRegs.gpr, x86 numbering. */
static const int isa_gregs[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                  REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                  REG_R12, REG_R13, REG_R14, REG_R15};

int isa_rewrite_handle_sigtrap(IsaTable* t, void* context) {
    ucontext_t* uc = context;
    greg_t* g = uc->uc_mcontext.gregs;
    const uintptr_t address = (uintptr_t)g[REG_RIP] - 1; /* RIP is past the int3 */
    const int index = int3_site_at(t, address);
    if (index < 0)
        return 0;
    const IsaSite* site = &t->sites[index];
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    for (int i = 0; i < 16; ++i)
        regs.gpr[i] = (uint64_t)g[isa_gregs[i]];
    regs.rflags = (uint64_t)g[REG_EFL];
    struct _libc_xmmreg* xmm = uc->uc_mcontext.fpregs ? uc->uc_mcontext.fpregs->_xmm : NULL;
    if (is_sse4a(site->kind)) {
        if (!xmm)
            return 0; /* no FP state in the frame: leave it to the fault report */
        memcpy(regs.xmm[0], &xmm[site->dst], 16);
        if (site->src1 != ISA_NO_REG)
            memcpy(regs.xmm[1], &xmm[site->src1], 16);
    }
    uintptr_t next = 0;
    if (!isa_rewrite_trap(t, address, &regs, &next))
        return 0;
    for (int i = 0; i < 16; ++i)
        g[isa_gregs[i]] = (greg_t)regs.gpr[i];
    g[REG_EFL] = (greg_t)regs.rflags;
    g[REG_RIP] = (greg_t)next;
    if (is_sse4a(site->kind))
        memcpy(&xmm[site->dst], regs.xmm[0], 8); /* low quadword only */
    return 1;
}

int isa_rewrite_site_of_stub(const IsaTable* t, uintptr_t rip) {
    if (!t || !t->stubs || rip < (uintptr_t)t->stubs)
        return -1;
    const uintptr_t at = rip - (uintptr_t)t->stubs;
    return at < isa_stub_area_size(t) ? (int)(at / ISA_STUB_BYTES) : -1;
}

const char* isa_kind_name(uint8_t kind) {
    static const char* const names[] = {"?",         "movbe",     "movbe", "lzcnt",  "tzcnt",
                                        "andn",      "bextr",     "blsi",  "blsmsk", "blsr",
                                        "vcvtph2ps", "vcvtps2ph", "extrq", "insertq"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "?";
}

/* Report order: more hits first, then lower offset. */
static int ranks_before(const IsaTable* t, uint32_t a, uint32_t b) {
    return t->hits[a] > t->hits[b] ||
           (t->hits[a] == t->hits[b] && t->sites[a].offset < t->sites[b].offset);
}

void isa_rewrite_report(const IsaTable* t, FILE* out) {
    fprintf(out, "ISA: most executed sites:\n");
    int64_t prev = -1; /* last printed site; each pass takes the best one ranked after it */
    for (int line = 0; line < 10; line++) {
        int64_t best = -1;
        for (uint32_t i = 0; i < t->count; i++) {
            if (t->hits[i] == 0 || (prev >= 0 && !ranks_before(t, (uint32_t)prev, i)))
                continue;
            if (best < 0 || ranks_before(t, i, (uint32_t)best))
                best = i;
        }
        if (best < 0)
            return;
        fprintf(out, "  0x%llx %s %llu\n", (unsigned long long)t->sites[best].offset,
                isa_kind_name(t->sites[best].kind), (unsigned long long)t->hits[best]);
        prev = best;
    }
}
