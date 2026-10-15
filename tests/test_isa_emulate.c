// SPDX-License-Identifier: GPL-2.0-or-later
/* isa_emulate (src/isa_emulate.c): MOVBE, LZCNT, BMI1 and SSE4a (docs/10 §5).
 *
 * Two oracles. (1) An independent reference model written bit by bit, with no __builtin_*, and
 * hand-made vectors: runs everywhere, including the development VM, which has none of these
 * instructions. (2) The real instruction, run through inline asm and compared register by
 * register and flag by flag: only when CPUID says the host has the set (otherwise SKIP; the
 * maintainer's AMD test machine runs it, tools/sotc-pc-test.sh).
 *
 * Structure and edge values adapted from gow3 91b6e61 tests/test_isa_emulate.c; the model and the
 * SSE4a cases are new. No -mbmi/-mlzcnt flags: native instructions only sit behind CPUID checks. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_check.h"
#include "isa_emulate.h"

static const uint64_t EDGES[] = {0,
                                 1,
                                 2,
                                 0x7f,
                                 0x80,
                                 0xff,
                                 0x7fff,
                                 0x8000,
                                 0xffff,
                                 0x7fffffff,
                                 0x80000000,
                                 0xffffffff,
                                 0x7fffffffffffffff,
                                 0x8000000000000000,
                                 0xffffffffffffffff};
static const uint64_t FIELDS[] = {0,  1,  7,  8,  15, 16,
                                  31, 32, 63, 64, 255}; /* BEXTR start, length */
enum {
    EDGE_COUNT = sizeof EDGES / sizeof EDGES[0],
    FIELD_COUNT = sizeof FIELDS / sizeof FIELDS[0],
    RANDOM_INPUTS = 20000,
};
static const uint64_t RAX_START = 0x1122334455667788ull;
static const uint64_t FLAG_STARTS[] = {0, 0xcd5};

static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static uint64_t xorshift64(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static unsigned long cases_run, native_run;

static void fail(const char* what, uint8_t kind, int width, uint64_t a, uint64_t b,
                 uint64_t expected, uint64_t got) {
    printf("isa-emulate-test FAILED: %s kind %u width %d inputs %#" PRIx64 ", %#" PRIx64
           ": expected %#" PRIx64 " got %#" PRIx64 "\n",
           what, kind, width, a, b, expected, got);
    exit(1);
}

/* ---- the reference model: bit by bit, no builtins ---- */

static uint64_t mask_of(int width) {
    return width == 64 ? ~0ull : (1ull << width) - 1;
}
static int bit_of(uint64_t x, int n) {
    return (int)((x >> n) & 1);
}
static int model_lzcnt(uint64_t x, int width) {
    int n = 0;
    for (int b = width - 1; b >= 0 && !bit_of(x, b); --b)
        ++n;
    return n;
}
static int model_tzcnt(uint64_t x, int width) {
    int n = 0;
    for (int b = 0; b < width && !bit_of(x, b); ++b)
        ++n;
    return n;
}
static uint64_t model_bswap(uint64_t x, int width) {
    uint64_t r = 0;
    for (int i = 0; i < width / 8; ++i)
        r |= ((x >> (8 * i)) & 0xff) << (width - 8 - 8 * i);
    return r;
}
static uint64_t model_bextr(uint64_t src, uint64_t control, int width) {
    const int start = (int)(control & 0xff), length = (int)((control >> 8) & 0xff);
    uint64_t r = 0;
    for (int i = 0; i < length && i < 64; ++i)
        if (start + i < width && bit_of(src, start + i))
            r |= 1ull << i;
    return r;
}
static uint64_t sign_bit(int width) {
    return 1ull << (width - 1);
}

typedef struct {
    uint64_t result, flags;
} Expected;

/* Expected result and defined flags of a register-form site, from the model. */
static Expected model(uint8_t kind, int width, uint64_t src1, uint64_t src2) {
    const uint64_t m = mask_of(width);
    const uint64_t a = src1 & m, b = src2 & m;
    Expected e = {0, 0};
    switch (kind) {
    case ISA_LZCNT:
        e.result = (uint64_t)model_lzcnt(a, width);
        e.flags = (a == 0 ? ISA_CF : 0) | (e.result == 0 ? ISA_ZF : 0);
        break;
    case ISA_TZCNT:
        e.result = (uint64_t)model_tzcnt(a, width);
        e.flags = (a == 0 ? ISA_CF : 0) | (e.result == 0 ? ISA_ZF : 0);
        break;
    case ISA_ANDN:
        e.result = (~a) & b & m;
        e.flags = (e.result == 0 ? ISA_ZF : 0) | ((e.result & sign_bit(width)) ? ISA_SF : 0);
        break;
    case ISA_BEXTR:
        e.result = model_bextr(a, b, width);
        e.flags = e.result == 0 ? ISA_ZF : 0;
        break;
    case ISA_BLSI:
        e.result = a & ((~a + 1) & m);
        e.flags = (e.result == 0 ? ISA_ZF : 0) | ((e.result & sign_bit(width)) ? ISA_SF : 0) |
                  (a != 0 ? ISA_CF : 0);
        break;
    case ISA_BLSMSK:
        e.result = (a ^ ((a - 1) & m)) & m;
        e.flags = ((e.result & sign_bit(width)) ? ISA_SF : 0) | (a == 0 ? ISA_CF : 0);
        break;
    case ISA_BLSR:
        e.result = a & ((a - 1) & m);
        e.flags = (e.result == 0 ? ISA_ZF : 0) | ((e.result & sign_bit(width)) ? ISA_SF : 0) |
                  (a == 0 ? ISA_CF : 0);
        break;
    default:
        abort();
    }
    return e;
}

/* ---- native oracle (only behind CPUID) ---- */

static CpuFeatureWords host;
static int native_has(unsigned set) {
    char names[64];
    return (cpu_missing_rewritable(host, names, sizeof names) & set) == 0;
}

/* The red zone below rsp is stepped over before pushfq. */
#define FLAGS_OUT "\n\tleaq -128(%%rsp), %%rsp\n\tpushfq\n\tpopq %[f]\n\tleaq 128(%%rsp), %%rsp"
#define NATIVE1(mn, sz)                                                                            \
    __asm__(mn " %" sz "[s1], %" sz "[d]" FLAGS_OUT                                                \
            : [d] "+r"(dst), [f] "=r"(flags)                                                       \
            : [s1] "r"(src1)                                                                       \
            : "cc")
#define NATIVE2(mn, sz)                                                                            \
    __asm__(mn " %" sz "[s2], %" sz "[s1], %" sz "[d]" FLAGS_OUT                                   \
            : [d] "+r"(dst), [f] "=r"(flags)                                                       \
            : [s1] "r"(src1), [s2] "r"(src2)                                                       \
            : "cc")
#define BY_WIDTH(macro, mn)                                                                        \
    do {                                                                                           \
        if (width == 32)                                                                           \
            macro(mn "l", "k");                                                                    \
        else                                                                                       \
            macro(mn "q", "q");                                                                    \
    } while (0)
#define BY_WIDTH16(macro, mn)                                                                      \
    do {                                                                                           \
        if (width == 16)                                                                           \
            macro(mn "w", "w");                                                                    \
        else                                                                                       \
            BY_WIDTH(macro, mn);                                                                   \
    } while (0)

static uint64_t native_register(uint8_t kind, int width, uint64_t src1, uint64_t src2,
                                uint64_t* flags_out) {
    uint64_t dst = RAX_START, flags = 0;
    switch (kind) {
    case ISA_LZCNT:
        BY_WIDTH16(NATIVE1, "lzcnt");
        break;
    case ISA_TZCNT:
        BY_WIDTH16(NATIVE1, "tzcnt");
        break;
    case ISA_BLSI:
        BY_WIDTH(NATIVE1, "blsi");
        break;
    case ISA_BLSMSK:
        BY_WIDTH(NATIVE1, "blsmsk");
        break;
    case ISA_BLSR:
        BY_WIDTH(NATIVE1, "blsr");
        break;
    case ISA_ANDN:
        BY_WIDTH(NATIVE2, "andn");
        break;
    case ISA_BEXTR:
        BY_WIDTH(NATIVE2, "bextr");
        break;
    default:
        abort();
    }
    *flags_out = flags;
    return dst;
}

static unsigned set_of(uint8_t kind) {
    return kind == ISA_LZCNT ? CPU_SET_LZCNT : CPU_SET_BMI1;
}

/* ---- register forms: dst = rax, src1 = rcx, src2 = rdx ---- */

static IsaSite register_site(uint8_t kind, int width) {
    IsaSite site;
    memset(&site, 0, sizeof site);
    site.kind = kind;
    site.width = (uint8_t)width;
    site.dst = 0;
    site.src1 = 1;
    site.src2 = (kind == ISA_ANDN || kind == ISA_BEXTR) ? 2 : ISA_NO_REG;
    site.mem_slot = ISA_MEM_NONE;
    site.base = site.index = ISA_NO_REG;
    site.scale = 1;
    return site;
}

static void check_register(uint8_t kind, int width, uint64_t src1, uint64_t src2, unsigned round) {
    const Expected want = model(kind, width, src1, src2);
    uint64_t expected_dst = want.result;
    if (width == 16) /* 16-bit writes keep bits 16-63 */
        expected_dst = (RAX_START & ~0xffffull) | want.result;
    /* 32-bit writes zero-extend: the model result already is < 2^32 */
    IsaSite site = register_site(kind, width);
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    for (int r = 0; r < 16; ++r)
        regs.gpr[r] = 0x0101010101010101ull * (uint64_t)r;
    regs.gpr[0] = RAX_START;
    regs.gpr[1] = src1;
    regs.gpr[2] = src2;
    const uint64_t flags_start = FLAG_STARTS[round & 1];
    regs.rflags = flags_start;
    isa_emulate(&site, &regs);
    const uint64_t defined = isa_defined_flags(kind);
    ++cases_run;
    if (regs.gpr[0] != expected_dst)
        fail("result", kind, width, src1, src2, expected_dst, regs.gpr[0]);
    if ((regs.rflags & defined) != want.flags)
        fail("defined flags", kind, width, src1, src2, want.flags, regs.rflags & defined);
    if ((regs.rflags ^ flags_start) & ~defined)
        fail("undefined flags touched", kind, width, src1, src2, flags_start, regs.rflags);
    if (regs.gpr[1] != src1 || regs.gpr[2] != src2)
        fail("sources", kind, width, src1, src2, src1 ^ src2, regs.gpr[1] ^ regs.gpr[2]);
    for (int r = 3; r < 16; ++r)
        if (regs.gpr[r] != 0x0101010101010101ull * (uint64_t)r)
            fail("other register", kind, width, src1, src2, (uint64_t)r, regs.gpr[r]);
    if (native_has(set_of(kind))) {
        uint64_t native_flags;
        const uint64_t native_dst = native_register(kind, width, src1, src2, &native_flags);
        ++native_run;
        if (native_dst != regs.gpr[0])
            fail("native result", kind, width, src1, src2, native_dst, regs.gpr[0]);
        if ((native_flags ^ regs.rflags) & defined)
            fail("native flags", kind, width, src1, src2, native_flags & defined,
                 regs.rflags & defined);
    }
}

static void check_registers(void) {
    static const uint8_t kinds[] = {ISA_LZCNT, ISA_TZCNT,  ISA_ANDN, ISA_BEXTR,
                                    ISA_BLSI,  ISA_BLSMSK, ISA_BLSR};
    static const int widths[] = {16, 32, 64};
    unsigned round = 0;
    for (size_t k = 0; k < sizeof kinds; ++k) {
        for (size_t w = 0; w < 3; ++w) {
            const uint8_t kind = kinds[k];
            const int width = widths[w];
            if (width == 16 && kind != ISA_LZCNT && kind != ISA_TZCNT)
                continue; /* the VEX forms are 32/64 only */
            for (int a = 0; a < EDGE_COUNT; ++a) {
                if (kind == ISA_BEXTR) {
                    for (int s = 0; s < FIELD_COUNT; ++s)
                        for (int l = 0; l < FIELD_COUNT; ++l)
                            check_register(kind, width, EDGES[a], FIELDS[s] | FIELDS[l] << 8,
                                           round++);
                } else {
                    for (int b = 0; b < EDGE_COUNT; ++b)
                        check_register(kind, width, EDGES[a], EDGES[b], round++);
                }
            }
            for (int i = 0; i < RANDOM_INPUTS; ++i) {
                const uint64_t a = xorshift64(), b = xorshift64();
                /* BEXTR control: random start/length most of the time, edge values otherwise */
                check_register(kind, width, a, kind == ISA_BEXTR ? (b & 0xffff) : b, round++);
            }
        }
    }
}

/* ---- memory forms: MOVBE load/store, an LZCNT source and an ANDN source in memory ---- */

static void check_memory(void) {
    unsigned char image[256];
    for (size_t i = 0; i < sizeof image; ++i)
        image[i] = (unsigned char)(i * 37 + 11);
    isa_emulate_set_image(image);
    static const int widths[] = {16, 32, 64};
    for (size_t w = 0; w < 3; ++w) {
        const int width = widths[w];
        const int bytes = width / 8;
        /* base + index * scale + disp -> image + 0x20 */
        IsaSite load;
        memset(&load, 0, sizeof load);
        load.kind = ISA_MOVBE_LOAD;
        load.width = (uint8_t)width;
        load.dst = 3; /* rbx */
        load.src1 = ISA_NO_REG;
        load.mem_slot = ISA_MEM_SRC;
        load.base = 6;  /* rsi */
        load.index = 7; /* rdi */
        load.scale = 4;
        load.disp = 0x10;
        IsaRegs regs;
        memset(&regs, 0, sizeof regs);
        regs.gpr[3] = 0xAAAAAAAAAAAAAAAAull;
        regs.gpr[6] = (uint64_t)(uintptr_t)image;
        regs.gpr[7] = 4; /* 4 * 4 = 16: image + 0x10 + 0x10 = image + 0x20 */
        isa_emulate(&load, &regs);
        uint64_t raw = 0;
        memcpy(&raw, image + 0x20, (size_t)bytes);
        uint64_t expected = model_bswap(raw, width);
        if (width == 16)
            expected |= regs.gpr[3] & ~0xffffull; /* 16-bit load keeps the rest of rbx */
        ++cases_run;
        if (regs.gpr[3] != (width == 16
                                ? ((0xAAAAAAAAAAAAAAAAull & ~0xffffull) | model_bswap(raw, 16))
                                : expected))
            fail("movbe load", ISA_MOVBE_LOAD, width, raw, 0, expected, regs.gpr[3]);

        /* store: reg -> [rip-relative image + 0x40]; neighbours untouched */
        IsaSite store = load;
        store.kind = ISA_MOVBE_STORE;
        store.dst = ISA_NO_REG;
        store.src1 = 3;
        store.mem_slot = ISA_MEM_DST;
        store.mem_flags = ISA_MEM_RIP_RELATIVE;
        store.base = store.index = ISA_NO_REG;
        store.scale = 1;
        store.disp = 0x40;
        unsigned char before[16];
        memcpy(before, image + 0x38, sizeof before);
        regs.gpr[3] = 0x0807060504030201ull;
        isa_emulate(&store, &regs);
        uint64_t written = 0;
        memcpy(&written, image + 0x40, (size_t)bytes);
        ++cases_run;
        if (written != model_bswap(regs.gpr[3] & mask_of(width), width))
            fail("movbe store", ISA_MOVBE_STORE, width, regs.gpr[3], 0,
                 model_bswap(regs.gpr[3] & mask_of(width), width), written);
        if (memcmp(before, image + 0x38, 8) ||
            memcmp(before + 8 + bytes, image + 0x40 + bytes, (size_t)(8 - bytes)))
            fail("movbe store neighbours", ISA_MOVBE_STORE, width, 0, 0, 0, 1);
    }
    /* LZCNT with the source in memory (src1 = NO_REG): 32-bit zero-extends dst. */
    IsaSite lz;
    memset(&lz, 0, sizeof lz);
    lz.kind = ISA_LZCNT;
    lz.width = 32;
    lz.dst = 0;
    lz.src1 = ISA_NO_REG;
    lz.src2 = ISA_NO_REG;
    lz.mem_slot = ISA_MEM_SRC;
    lz.base = 6;
    lz.index = ISA_NO_REG;
    lz.scale = 0;
    lz.disp = 8;
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    regs.gpr[0] = ~0ull;
    regs.gpr[6] = (uint64_t)(uintptr_t)image;
    const uint32_t word = 0x00010000u; /* bit 16 set: 15 leading zeros in 32 bits */
    memcpy(image + 8, &word, 4);
    isa_emulate(&lz, &regs);
    ++cases_run;
    if (regs.gpr[0] != 15)
        fail("lzcnt memory", ISA_LZCNT, 32, word, 0, 15, regs.gpr[0]);
    isa_emulate_set_image(NULL);
}

/* ---- SSE4a: EXTRQ and INSERTQ on the spilled low/high quadwords ---- */

static uint64_t model_extrq(uint64_t d, int length, int index) {
    const int len = length ? length : 64;
    uint64_t r = 0;
    for (int i = 0; i < len; ++i)
        if (index + i < 64 && bit_of(d, index + i))
            r |= 1ull << i;
    return r;
}
static uint64_t model_insertq(uint64_t d, uint64_t src, int length, int index) {
    const int len = length ? length : 64;
    uint64_t r = d;
    for (int i = 0; i < len; ++i) {
        if (index + i >= 64)
            break;
        r &= ~(1ull << (index + i));
        if (bit_of(src, i))
            r |= 1ull << (index + i);
    }
    return r;
}

static IsaSite sse4a_site(uint8_t kind, int immediate, int length, int index) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.dst = 3;
    s.src1 = immediate && kind == ISA_EXTRQ ? ISA_NO_REG : 5;
    s.src2 = ISA_NO_REG;
    s.base = s.index = ISA_NO_REG;
    s.scale = 0;
    if (immediate) {
        s.mem_flags = ISA_FLAG_IMM;
        s.disp = (int64_t)(length | index << 8);
    }
    return s;
}

static void check_sse4a_one(uint8_t kind, int immediate, uint64_t dst_lo, uint64_t src_lo,
                            int length, int index) {
    IsaSite site = sse4a_site(kind, immediate, length, index);
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    for (int r = 0; r < 16; ++r)
        regs.gpr[r] = 0x0101010101010101ull * (uint64_t)r;
    regs.rflags = 0xcd5;
    const uint64_t dst_hi = 0xdeadbeefcafef00dull, src_hi_start = 0x1234567890abcdefull;
    regs.xmm[0][0] = dst_lo;
    regs.xmm[0][1] = dst_hi;
    regs.xmm[1][0] = src_lo;
    /* Register forms carry length/index in the source register image. */
    uint64_t src_hi = src_hi_start;
    uint64_t ctrl_lo = src_lo;
    if (!immediate && kind == ISA_EXTRQ) {
        ctrl_lo = (uint64_t)(length & 0x3f) | (uint64_t)(index & 0x3f) << 8;
        regs.xmm[1][0] = ctrl_lo;
    }
    if (!immediate && kind == ISA_INSERTQ) {
        src_hi = (uint64_t)(length & 0x3f) | (uint64_t)(index & 0x3f) << 8;
    }
    regs.xmm[1][1] = src_hi;
    const IsaRegs snapshot = regs;
    isa_emulate(&site, &regs);
    ++cases_run;
    const uint64_t expected = kind == ISA_EXTRQ ? model_extrq(dst_lo, length, index)
                                                : model_insertq(dst_lo, src_lo, length, index);
    if (regs.xmm[0][0] != expected)
        fail(kind == ISA_EXTRQ ? "extrq" : "insertq", kind, immediate, dst_lo,
             (uint64_t)(length | index << 8), expected, regs.xmm[0][0]);
    if (regs.xmm[0][1] != dst_hi) /* the high quadword is undefined in hardware; kept here */
        fail("sse4a high quadword", kind, immediate, dst_lo, 0, dst_hi, regs.xmm[0][1]);
    if (regs.xmm[1][0] != snapshot.xmm[1][0] || regs.xmm[1][1] != snapshot.xmm[1][1])
        fail("sse4a source", kind, immediate, src_lo, 0, snapshot.xmm[1][0], regs.xmm[1][0]);
    if (regs.rflags != snapshot.rflags)
        fail("sse4a flags", kind, immediate, 0, 0, snapshot.rflags, regs.rflags);
    for (int r = 0; r < 16; ++r)
        if (regs.gpr[r] != snapshot.gpr[r])
            fail("sse4a gpr", kind, immediate, (uint64_t)r, 0, snapshot.gpr[r], regs.gpr[r]);
}

/* Native SSE4a, register forms only (the immediates must be literals): xmm0 = dst, xmm1 = src. */
static uint64_t native_extrq_reg(uint64_t dst, uint64_t control) {
    uint64_t out;
    __asm__ volatile("movq %1, %%xmm0\n\tmovq %2, %%xmm1\n\textrq %%xmm1, %%xmm0\n\tmovq %%xmm0, %0"
                     : "=r"(out)
                     : "r"(dst), "r"(control)
                     : "xmm0", "xmm1");
    return out;
}
static uint64_t native_insertq_reg(uint64_t dst, uint64_t src, uint64_t control) {
    uint64_t out;
    __asm__ volatile(
        "movq %1, %%xmm0\n\tmovq %2, %%xmm1\n\tmovq %3, %%xmm2\n\tpunpcklqdq %%xmm2, %%xmm1\n\t"
        "insertq %%xmm1, %%xmm0\n\tmovq %%xmm0, %0"
        : "=r"(out)
        : "r"(dst), "r"(src), "r"(control)
        : "xmm0", "xmm1", "xmm2");
    return out;
}

static void check_sse4a(void) {
    /* Hand-made vectors (AMD APM vol. 4 semantics): bit field [index + length - 1 : index]. */
    IsaRegs regs;
    IsaSite site = sse4a_site(ISA_EXTRQ, 1, 8, 4);
    memset(&regs, 0, sizeof regs);
    regs.xmm[0][0] = 0x1122334455667788ull;
    isa_emulate(&site, &regs);
    if (regs.xmm[0][0] != 0x78)
        fail("vector extrq 8,4", ISA_EXTRQ, 1, 0x1122334455667788ull, 0, 0x78, regs.xmm[0][0]);
    site = sse4a_site(ISA_EXTRQ, 1, 0, 0); /* length 0 = 64 */
    regs.xmm[0][0] = 0x1122334455667788ull;
    isa_emulate(&site, &regs);
    if (regs.xmm[0][0] != 0x1122334455667788ull)
        fail("vector extrq 64,0", ISA_EXTRQ, 1, 0, 0, 0x1122334455667788ull, regs.xmm[0][0]);
    site = sse4a_site(ISA_INSERTQ, 1, 8, 16);
    memset(&regs, 0, sizeof regs);
    regs.xmm[0][0] = ~0ull;
    regs.xmm[1][0] = 0xab;
    isa_emulate(&site, &regs);
    if (regs.xmm[0][0] != 0xffffffffffabffffull)
        fail("vector insertq 8,16", ISA_INSERTQ, 1, 0, 0, 0xffffffffffabffffull, regs.xmm[0][0]);

    static const int FIELDS_LEN[] = {0, 1, 2, 7, 8, 16, 31, 32, 33, 63};
    static const int FIELDS_IDX[] = {0, 1, 4, 8, 15, 16, 31, 32, 48, 56};
    for (size_t l = 0; l < sizeof FIELDS_LEN / sizeof *FIELDS_LEN; ++l)
        for (size_t i = 0; i < sizeof FIELDS_IDX / sizeof *FIELDS_IDX; ++i) {
            const int length = FIELDS_LEN[l], index = FIELDS_IDX[i];
            if (length != 0 && index + length > 64)
                continue; /* undefined in the manual */
            if (length == 0 && index != 0)
                continue;
            for (int e = 0; e < EDGE_COUNT; ++e)
                for (int imm = 0; imm < 2; ++imm) {
                    check_sse4a_one(ISA_EXTRQ, imm, EDGES[e], 0, length, index);
                    check_sse4a_one(ISA_INSERTQ, imm, EDGES[e], EDGES[(e + 5) % EDGE_COUNT], length,
                                    index);
                }
            for (int r = 0; r < 50; ++r) {
                const uint64_t d = xorshift64(), s = xorshift64();
                check_sse4a_one(ISA_EXTRQ, r & 1, d, s, length, index);
                check_sse4a_one(ISA_INSERTQ, r & 1, d, s, length, index);
                if (native_has(CPU_SET_SSE4A)) {
                    const uint64_t ctrl = (uint64_t)length | (uint64_t)index << 8;
                    const uint64_t native_e = native_extrq_reg(d, ctrl);
                    const uint64_t native_i = native_insertq_reg(d, s, ctrl);
                    native_run += 2;
                    if (native_e != model_extrq(d, length, index))
                        fail("native extrq", ISA_EXTRQ, 0, d, ctrl, native_e,
                             model_extrq(d, length, index));
                    if (native_i != model_insertq(d, s, length, index))
                        fail("native insertq", ISA_INSERTQ, 0, d, ctrl, native_i,
                             model_insertq(d, s, length, index));
                }
            }
        }
}

static void check_defined_flags(void) {
    /* The flag sets the tests compare must match the SDM list (docs/10 §5). */
    if (isa_defined_flags(ISA_LZCNT) != (ISA_CF | ISA_ZF) ||
        isa_defined_flags(ISA_TZCNT) != (ISA_CF | ISA_ZF) ||
        isa_defined_flags(ISA_BEXTR) != (ISA_ZF | ISA_CF | ISA_OF) ||
        isa_defined_flags(ISA_ANDN) != (ISA_SF | ISA_ZF | ISA_CF | ISA_OF) ||
        isa_defined_flags(ISA_EXTRQ) != 0 || isa_defined_flags(ISA_INSERTQ) != 0 ||
        isa_defined_flags(ISA_MOVBE_LOAD) != 0)
        fail("defined flags table", 0, 0, 0, 0, 0, 1);
}

int main(void) {
    host = cpu_feature_words();
    check_defined_flags();
    check_registers();
    check_memory();
    check_sse4a();
    char absent[64];
    cpu_missing_rewritable(host, absent, sizeof absent);
    printf(
        "PASS: ISA emulation (%lu cases against the model; %lu against the real instruction)%s%s\n",
        cases_run, native_run, absent[0] ? "; native oracle skipped for: " : "", absent);
    return 0;
}
