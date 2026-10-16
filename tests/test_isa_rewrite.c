// SPDX-License-Identifier: GPL-2.0-or-later
/* isa_rewrite (src/isa_rewrite.c): table parsing, stub emission, patching and the int3 trap,
 * with the generated stubs running real patched code in an RWX region.
 *
 * Snippets S1-S5 and the checks of patching modes follow gow3 91b6e61 tests/test_isa_stub.c;
 * the table-in-memory cases, the SSE4a snippets (S6-S9) and the validation cases are new. The
 * expectations come from a model, not from the instructions, because the development VM has
 * none of them: the patched code is the only way to run them there. */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <sys/mman.h>

#include "isa_rewrite.h"

typedef uint64_t(ABI* Snippet)(uint64_t a, uint64_t b, uint64_t c);
enum {
    PAGE = 4096,
    S1 = 0x000,
    S2 = 0x040,
    S3 = 0x080,
    S4 = 0x0c0,
    S5 = 0x100,
    S6 = 0x140,
    S7 = 0x180,
    S8 = 0x1c0,
    S9 = 0x200,
    S10 = 0x240
};
/* S10 (red zone): the canary rdx at [rsp-0x80], [rsp-0x78], [rsp-0x70], [rsp-0x68]; movq xmm0,rdi;
 * movq xmm1,rsi; extrq xmm0,xmm1 (the site, +30); then rax = OR of (slot ^ rdx): 0 when the stub
 * left the 128 bytes below the guest's rsp alone, as the SysV ABI promises a leaf function. */
static const uint8_t S10_CODE[] = {
    0x48, 0x89, 0x54, 0x24, 0x80, 0x48, 0x89, 0x54, 0x24, 0x88, 0x48, 0x89, 0x54, 0x24, 0x90,
    0x48, 0x89, 0x54, 0x24, 0x98, 0x66, 0x48, 0x0f, 0x6e, 0xc7, 0x66, 0x48, 0x0f, 0x6e, 0xce,
    0x66, 0x0f, 0x79, 0xc1,                   /* extrq xmm0,xmm1 */
    0x48, 0x8b, 0x44, 0x24, 0x80, 0x48, 0x31, 0xd0, /* mov rax,[rsp-0x80]; xor rax,rdx */
    0x48, 0x8b, 0x4c, 0x24, 0x88, 0x48, 0x31, 0xd1, 0x48, 0x09, 0xc8,
    0x48, 0x8b, 0x4c, 0x24, 0x90, 0x48, 0x31, 0xd1, 0x48, 0x09, 0xc8,
    0x48, 0x8b, 0x4c, 0x24, 0x98, 0x48, 0x31, 0xd1, 0x48, 0x09, 0xc8, 0xc3};

static const uint8_t
    S1_CODE[] = /* mov [rsp-8],0xdeadbeef; mov ecx,edi; blsr ecx,ecx; mov rdx,[rsp-8]; mov
                   [rsi+8],rdx; pushfq; pop [rsi]; mov eax,ecx; ret */
    {0x48, 0xc7, 0x44, 0x24, 0xf8, 0xef, 0xbe, 0xad, 0xde, 0x89, 0xf9, 0xc4, 0xe2, 0x70, 0xf3, 0xc9,
     0x48, 0x8b, 0x54, 0x24, 0xf8, 0x48, 0x89, 0x56, 0x08, 0x9c, 0x8f, 0x06, 0x89, 0xc8, 0xc3};
static const uint8_t S2_CODE[] = {0x89, 0xf1, 0x0f, 0x38, 0xf1,
                                  0x0f, 0x89, 0xc8, 0xc3}; /* movbe [rdi],ecx */
static const uint8_t S3_CODE[] = {0x89, 0xf9, 0xc4, 0xe2, 0x70, 0xf3, 0xc9, 0xc4,
                                  0xe2, 0x70, 0xf3, 0xc9, 0x89, 0xc8, 0xc3};
static const uint8_t S4_CODE[] = {0xf3, 0x0f, 0xbd, 0xc7, 0xc3}; /* lzcnt eax,edi; ret */
static const uint8_t S5_CODE[] = {0xf3, 0x0f, 0xbd, 0xc7, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xc3};
/* S6: movq xmm0,rdi; movq xmm1,rsi; extrq xmm0,xmm1; movq rax,xmm0; ret */
static const uint8_t S6_CODE[] = {0x66, 0x48, 0x0f, 0x6e, 0xc7, 0x66, 0x48, 0x0f, 0x6e, 0xce,
                                  0x66, 0x0f, 0x79, 0xc1, 0x66, 0x48, 0x0f, 0x7e, 0xc0, 0xc3};
/* S7: movq xmm0,rdi; pinsrq xmm0,rdx,1; movq xmm1,rsi; extrq xmm0,xmm1; pextrq rax,xmm0,1; ret */
static const uint8_t S7_CODE[] = {0x66, 0x48, 0x0f, 0x6e, 0xc7, 0x66, 0x48, 0x0f, 0x3a, 0x22,
                                  0xc2, 0x01, 0x66, 0x48, 0x0f, 0x6e, 0xce, 0x66, 0x0f, 0x79,
                                  0xc1, 0x66, 0x48, 0x0f, 0x3a, 0x16, 0xc0, 0x01, 0xc3};
/* S8: movq xmm0,rdi; movq xmm1,rsi; pinsrq xmm1,rdx,1; insertq xmm0,xmm1; movq rax,xmm0; ret */
static const uint8_t S8_CODE[] = {0x66, 0x48, 0x0f, 0x6e, 0xc7, 0x66, 0x48, 0x0f, 0x6e,
                                  0xce, 0x66, 0x48, 0x0f, 0x3a, 0x22, 0xca, 0x01, 0xf2,
                                  0x0f, 0x79, 0xc1, 0x66, 0x48, 0x0f, 0x7e, 0xc0, 0xc3};

static unsigned char *region, *image, *stubs;
static unsigned char pristine[PAGE];

static void fail_at(int line) {
    fprintf(stderr, "isa-rewrite-test FAILED at line %d\n", line);
    exit(1);
}
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond))                                                                               \
            fail_at(__LINE__);                                                                     \
    } while (0)

/* ---- model of the expected values (bit by bit) ---- */

static uint32_t model_blsr(uint32_t x) {
    return x & (x - 1);
}
static uint64_t model_extrq(uint64_t d, int length, int index) {
    const int len = length ? length : 64;
    uint64_t r = 0;
    for (int i = 0; i < len; ++i)
        if (index + i < 64 && ((d >> (index + i)) & 1))
            r |= 1ull << i;
    return r;
}
static uint64_t model_insertq(uint64_t d, uint64_t src, int length, int index) {
    const int len = length ? length : 64;
    uint64_t r = d;
    for (int i = 0; i < len && index + i < 64; ++i) {
        r &= ~(1ull << (index + i));
        if ((src >> i) & 1)
            r |= 1ull << (index + i);
    }
    return r;
}
static int model_lzcnt32(uint32_t x) {
    int n = 0;
    for (int b = 31; b >= 0 && !((x >> b) & 1); --b)
        ++n;
    return n;
}

/* ---- tables ---- */

static IsaSite site_at(uint64_t offset, uint8_t length, uint8_t kind, uint8_t mode, uint8_t dst,
                       uint8_t src1) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.offset = offset;
    s.length = length;
    s.kind = kind;
    s.width = (kind == ISA_EXTRQ || kind == ISA_INSERTQ) ? 0 : 32;
    s.mode = mode;
    s.dst = dst;
    s.src1 = src1;
    s.src2 = ISA_NO_REG;
    s.mem_slot = ISA_MEM_NONE;
    s.base = ISA_NO_REG;
    s.index = ISA_NO_REG;
    s.scale = 0;
    memcpy(s.original, pristine + offset, length);
    return s;
}

static IsaTable* table_of(const IsaSite* sites, uint32_t count) {
    IsaTable* t = calloc(1, sizeof *t);
    t->sites = calloc(count ? count : 1, sizeof *sites);
    t->hits = calloc(count ? count : 1, sizeof *t->hits);
    memcpy(t->sites, sites, count * sizeof *sites);
    t->count = count;
    t->image_size = PAGE;
    return t;
}

static void relocate(IsaSite* s, uint64_t from, uint8_t length) {
    s->relocated_length = length;
    memcpy(s->relocated, pristine + from, length);
}

/* All snippet sites, ascending by offset. */
static IsaTable* snippet_table(void) {
    IsaSite sites[12];
    unsigned n = 0;
    sites[n++] = site_at(S1 + 0x0b, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[n] = site_at(S2 + 2, 4, ISA_MOVBE_STORE, ISA_MODE_JMP_RELOCATE, ISA_NO_REG, 1);
    sites[n].mem_slot = ISA_MEM_DST;
    sites[n].base = 7;
    sites[n].scale = 1;
    relocate(&sites[n++], S2 + 6, 2);
    sites[n++] = site_at(S3 + 2, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[n++] = site_at(S3 + 7, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[n++] = site_at(S4, 4, ISA_LZCNT, ISA_MODE_INT3, 0, 7);
    sites[n] = site_at(S5, 4, ISA_LZCNT, ISA_MODE_JMP_RELOCATE, 0, 7);
    relocate(&sites[n++], S5 + 4, 15);
    sites[n] = site_at(S6 + 10, 4, ISA_EXTRQ, ISA_MODE_JMP_RELOCATE, 0, 1);
    relocate(&sites[n++], S6 + 14, 5);
    sites[n] = site_at(S7 + 17, 4, ISA_EXTRQ, ISA_MODE_JMP_RELOCATE, 0, 1);
    relocate(&sites[n++], S7 + 21, 7);
    sites[n] = site_at(S8 + 17, 4, ISA_INSERTQ, ISA_MODE_JMP_RELOCATE, 0, 1);
    relocate(&sites[n++], S8 + 21, 5);
    sites[n++] = site_at(S9 + 10, 4, ISA_EXTRQ, ISA_MODE_INT3, 0, 1); /* same code as S6, trapped */
    return table_of(sites, n);
}

enum { SITE_S1, SITE_S2, SITE_S3A, SITE_S3B, SITE_S4, SITE_S5, SITE_S6, SITE_S7, SITE_S8, SITE_S9 };

static void reset_region(void) {
    memcpy(image, pristine, PAGE);
    memset(stubs, 0, 4 * PAGE);
}

static int32_t rel32_at(const unsigned char* at) {
    int32_t v;
    memcpy(&v, at, 4);
    return v;
}

/* ---- running patched code ---- */

static void test_jmp_and_flags(void) {
    reset_region();
    IsaTable* t = snippet_table();
    const IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    CHECK(r.rewritten == 10 && r.jmp == 3 && r.relocated == 5 && r.int3 == 2 && r.skipped == 0);
    CHECK(t->image == image && t->stubs == stubs);
    unsigned char* site = image + S1 + 0x0b;
    CHECK(site[0] == 0xe9 && site + 5 + rel32_at(site + 1) == stubs); /* jmp to stub 0 */
    static const uint8_t prologue[] = {0x48, 0x8d, 0xa4, 0x24,
                                       0x80, 0xfe, 0xff, 0xff}; /* lea rsp,[rsp-0x180] */
    CHECK(memcmp(stubs, prologue, sizeof prologue) == 0);
    Snippet s1 = (Snippet)(void*)(image + S1);
    static const uint32_t inputs[] = {0, 1, 0xf0, 0x80000000, 0xffffffff, 0x12345678};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        uint64_t out[2] = {0, 0};
        const uint32_t in = inputs[i], want = model_blsr(in);
        const uint64_t result = s1(in, (uint64_t)(uintptr_t)out, 0);
        CHECK(result == want);
        const uint64_t flags =
            (in == 0 ? ISA_CF : 0) | (want == 0 ? ISA_ZF : 0) | ((want >> 31) ? ISA_SF : 0);
        CHECK(((out[0] ^ flags) & isa_defined_flags(ISA_BLSR)) == 0);
        CHECK(out[1] == 0xffffffffdeadbeefull); /* the red zone below rsp survived the stub */
    }
    CHECK(t->hits[SITE_S1] == sizeof inputs / sizeof inputs[0]);
    isa_table_free(t);
}

static void test_relocated_site(void) {
    reset_region();
    IsaTable* t = snippet_table();
    isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    unsigned char* site = image + S2 + 2;
    CHECK(site[0] == 0xe9 && site + 5 + rel32_at(site + 1) == stubs + SITE_S2 * ISA_STUB_BYTES &&
          site[5] == 0x90);
    uint32_t buffer = 0;
    Snippet s2 = (Snippet)(void*)(image + S2);
    const uint64_t result = s2((uint64_t)(uintptr_t)&buffer, 0x11223344, 0);
    CHECK(buffer == 0x44332211 && result == 0x11223344); /* movbe stored swapped; mov eax,ecx ran */
    isa_table_free(t);
}

static void test_adjacent_and_long_relocation(void) {
    reset_region();
    IsaTable* t = snippet_table();
    isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    Snippet s3 = (Snippet)(void*)(image + S3);
    CHECK(s3(0xf0, 0, 0) == model_blsr(model_blsr(0xf0)) && s3(0xffffffff, 0, 0) == 0xfffffffc);
    CHECK(t->hits[SITE_S3A] == 2 && t->hits[SITE_S3B] == 2);
    /* S5: lzcnt with 15 relocated bytes behind it (the longest instruction x86 allows) */
    Snippet s5 = (Snippet)(void*)(image + S5);
    CHECK(s5(0x00010000, 0, 0) == (uint64_t)model_lzcnt32(0x00010000) && s5(0, 0, 0) == 32);
    isa_table_free(t);
}

static void test_sse4a_patched(void) {
    reset_region();
    IsaTable* t = snippet_table();
    isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    Snippet s6 = (Snippet)(void*)(image + S6), s7 = (Snippet)(void*)(image + S7),
            s8 = (Snippet)(void*)(image + S8);
    static const uint64_t values[] = {0x1122334455667788ull, ~0ull, 0, 0x8000000000000001ull,
                                      0xdeadbeefcafef00dull};
    static const int lengths[] = {0, 1, 8, 16, 32, 63};
    static const int indexes[] = {0, 4, 16, 32, 56};
    for (size_t v = 0; v < sizeof values / sizeof *values; ++v)
        for (size_t l = 0; l < sizeof lengths / sizeof *lengths; ++l)
            for (size_t i = 0; i < sizeof indexes / sizeof *indexes; ++i) {
                const int length = lengths[l], index = indexes[i];
                if (length && index + length > 64)
                    continue;
                if (!length && index)
                    continue;
                const uint64_t control = (uint64_t)length | (uint64_t)index << 8;
                CHECK(s6(values[v], control, 0) == model_extrq(values[v], length, index));
                /* the high quadword of the destination survives (pextrq reads it back) */
                CHECK(s7(values[v], control, 0x0123456789abcdefull) == 0x0123456789abcdefull);
                /* INSERTQ takes length/index from the high quadword of the source register */
                CHECK(s8(values[v], 0x5a, control) ==
                      model_insertq(values[v], 0x5a, length, index));
            }
    CHECK(t->hits[SITE_S6] > 0 && t->hits[SITE_S7] > 0 && t->hits[SITE_S8] > 0);
    isa_table_free(t);
}

/* The stub of a rewritten site must not write the guest's red zone (SysV: the 128 bytes below
 * rsp belong to the leaf function). Found while comparing with ShadowOfTheColossus-PC, whose
 * patchers protect it explicitly; here the stub keeps its frame below the red zone. */
static void test_stub_keeps_red_zone(void) {
    reset_region();
    IsaSite site = site_at(S10 + 30, 4, ISA_EXTRQ, ISA_MODE_JMP_RELOCATE, 0, 1);
    relocate(&site, S10 + 34, 5);
    IsaTable* t = table_of(&site, 1);
    const IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    CHECK(r.rewritten == 1);
    Snippet s10 = (Snippet)(void*)(image + S10);
    CHECK(s10(0x1122334455667788ull, 8 | 4 << 8, 0xc0ffee0123456789ull) == 0);
    CHECK(t->hits[0] == 1);
    isa_table_free(t);
}

/* ---- int3 sites: a real SIGTRAP ---- */

static IsaTable* trap_table;
static void on_trap(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)info;
    if (!isa_rewrite_handle_sigtrap(trap_table, context))
        _exit(99); /* a trap that is not ours */
}

static void test_int3_sites(void) {
    reset_region();
    IsaTable* t = snippet_table();
    isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    CHECK(image[S4] == 0xcc && image[S9 + 10] == 0xcc);
    trap_table = t;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGTRAP, &sa, &old) == 0);
    Snippet s4 = (Snippet)(void*)(image + S4), s9 = (Snippet)(void*)(image + S9);
    CHECK(s4(0x00010000, 0, 0) == (uint64_t)model_lzcnt32(0x00010000)); /* eax = 15 via the trap */
    CHECK(s4(0, 0, 0) == 32 && t->hits[SITE_S4] == 2);
    CHECK(s9(0x1122334455667788ull, 8 | 4 << 8, 0) ==
          0x78); /* SSE4a through the signal frame's XMM */
    CHECK(t->hits[SITE_S9] == 1);
    sigaction(SIGTRAP, &old, NULL);
    /* the same lookup without a signal: an address that is not a site */
    uintptr_t next = 0;
    IsaRegs regs;
    memset(&regs, 0, sizeof regs);
    CHECK(isa_rewrite_trap(t, (uintptr_t)image + 1, &regs, &next) == 0 && next == 0);
    isa_table_free(t);
}

/* ---- apply: skipped, downgraded, unsupported, out of range ---- */

static void test_changed_bytes_are_skipped(void) {
    reset_region();
    IsaTable* t = snippet_table();
    image[S1 + 0x0b] ^= 0xff; /* a patch changed the site */
    const IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_BMI1);
    CHECK(r.skipped == 1 && r.rewritten == 2 && image[S1 + 0x0b] == (pristine[S1 + 0x0b] ^ 0xff));
    isa_table_free(t);
}

static void test_changed_next_instruction_downgrades(void) {
    reset_region();
    IsaTable* t = snippet_table();
    image[S2 + 6] = 0x90; /* the instruction the jmp would cover is not the copied one anymore */
    const IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_MOVBE);
    CHECK(r.int3 == 1 && r.relocated == 0 && image[S2 + 2] == 0xcc &&
          t->sites[SITE_S2].mode == ISA_MODE_INT3);
    isa_table_free(t);
}

static void test_unsupported_needed_writes_nothing(void) {
    reset_region();
    IsaSite sites[2];
    sites[0] = site_at(S1 + 0x0b, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    sites[1] = site_at(S1 + 0x10, 5, ISA_VCVTPH2PS, ISA_MODE_UNSUPPORTED, 0, 1);
    IsaTable* t = table_of(sites, 2);
    t->unsupported = 1;
    IsaReport r = isa_rewrite_apply(t, image, stubs, CPU_SET_BMI1 | CPU_SET_F16C);
    CHECK(r.unsupported_needed == 1 && r.rewritten == 0 && memcmp(image, pristine, PAGE) == 0);
    r = isa_rewrite_apply(t, image, stubs, CPU_SET_BMI1); /* F16C not needed here: patch the rest */
    CHECK(r.unsupported_needed == 0 && r.rewritten == 1 && image[S1 + 0x0b] == 0xe9);
    isa_table_free(t);
}

static void test_out_of_rel32_range_is_skipped(void) {
    reset_region();
    IsaTable* t = snippet_table();
    unsigned char* far_stubs = image + (3ull << 30); /* never touched: the site is skipped first */
    const IsaReport r = isa_rewrite_apply(t, image, far_stubs, CPU_SET_BMI1);
    CHECK(r.rewritten == 0 && r.skipped == 3 && memcmp(image, pristine, PAGE) == 0);
    isa_table_free(t);
}

static void test_stub_lookup_and_report(void) {
    reset_region();
    IsaTable* t = snippet_table();
    isa_rewrite_apply(t, image, stubs, CPU_SET_ALL);
    CHECK(isa_stub_area_size(t) == (size_t)t->count * ISA_STUB_BYTES);
    CHECK(isa_rewrite_site_of_stub(t, (uintptr_t)stubs) == 0);
    CHECK(isa_rewrite_site_of_stub(t, (uintptr_t)stubs + 3 * ISA_STUB_BYTES + 17) == 3);
    CHECK(isa_rewrite_site_of_stub(t, (uintptr_t)stubs + isa_stub_area_size(t)) == -1);
    CHECK(isa_rewrite_site_of_stub(t, (uintptr_t)stubs - 1) == -1);
    t->hits[SITE_S1] = 5;
    t->hits[SITE_S6] = 9;
    t->hits[SITE_S8] = 5; /* tie with S1: the lower offset first */
    char* text = NULL;
    size_t size = 0;
    FILE* f = open_memstream(&text, &size);
    isa_rewrite_report(t, f);
    fclose(f);
    CHECK(strstr(text,
                 "ISA: most executed sites:\n  0x14a extrq 9\n  0xb blsr 5\n  0x1d1 insertq 5\n") ==
          text);
    free(text);
    CHECK(strcmp(isa_kind_name(ISA_EXTRQ), "extrq") == 0 &&
          strcmp(isa_kind_name(ISA_INSERTQ), "insertq") == 0);
    CHECK(strcmp(isa_kind_name(0), "?") == 0 && strcmp(isa_kind_name(200), "?") == 0);
    isa_table_free(t);
}

/* ---- table parsing and validation ---- */

static unsigned char* serialize(const IsaSite* sites, uint32_t count, uint64_t image_size,
                                size_t* size) {
    *size = ISA_TABLE_HEADER_BYTES + (size_t)count * ISA_TABLE_RECORD_BYTES;
    unsigned char* data = calloc(1, *size);
    memcpy(data, ISA_TABLE_MAGIC, 8);
    memcpy(data + 8, &image_size, 8);
    memcpy(data + 48, &count, 4);
    memcpy(data + ISA_TABLE_HEADER_BYTES, sites, (size_t)count * ISA_TABLE_RECORD_BYTES);
    return data;
}

static int parses(const IsaSite* sites, uint32_t count) {
    size_t size;
    unsigned char* data = serialize(sites, count, PAGE, &size);
    IsaTable* t = isa_table_parse(data, size);
    free(data);
    const int ok = t != NULL;
    isa_table_free(t);
    return ok;
}

static void test_table_validation(void) {
    IsaSite good[2];
    good[0] = site_at(S1 + 0x0b, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    good[1] = site_at(S3 + 2, 5, ISA_BLSR, ISA_MODE_JMP, 1, 1);
    CHECK(parses(good, 2) && parses(good, 0));

    size_t size;
    unsigned char* data = serialize(good, 2, PAGE, &size);
    CHECK(isa_table_parse(data, size) != NULL);
    CHECK(isa_table_parse(data, size - 1) == NULL); /* truncated */
    CHECK(isa_table_parse(data, 10) == NULL);       /* no header */
    data[0] = 'X';                                  /* bad magic */
    CHECK(isa_table_parse(data, size) == NULL);
    free(data);

    IsaSite bad[2];
    memcpy(bad, good, sizeof good);
    bad[1].offset = bad[0].offset; /* not strictly ascending */
    CHECK(!parses(bad, 2));
    memcpy(bad, good, sizeof good);
    bad[0].length = 3; /* shorter than a 4-byte site */
    CHECK(!parses(bad, 1));
    memcpy(bad, good, sizeof good);
    bad[0].kind = ISA_KIND_LAST + 1;
    CHECK(!parses(bad, 1));
    memcpy(bad, good, sizeof good);
    bad[0].dst = 16; /* not a register */
    CHECK(!parses(bad, 1));
    memcpy(bad, good, sizeof good);
    bad[0].offset = PAGE - 2; /* the site leaves the image */
    CHECK(!parses(bad, 1));
    memcpy(bad, good, sizeof good);
    bad[0].mode = ISA_MODE_JMP;
    bad[0].length = 4; /* a 4-byte site cannot be a 5-byte jmp */
    CHECK(!parses(bad, 1));
    memcpy(bad, good, sizeof good);
    bad[0].width = 24;
    CHECK(!parses(bad, 1));
    /* Audit finding 3: a RIP-relative operand must point inside the image. */
    memcpy(bad, good, sizeof good);
    bad[0].kind = ISA_MOVBE_LOAD;
    bad[0].width = 32;
    bad[0].mem_slot = ISA_MEM_SRC;
    bad[0].mem_flags = ISA_MEM_RIP_RELATIVE;
    bad[0].disp = 0x100;
    CHECK(parses(bad, 1));
    bad[0].disp = PAGE; /* one past the end */
    CHECK(!parses(bad, 1));
    bad[0].disp = -1;
    CHECK(!parses(bad, 1));
    /* SSE4a records: registers 0-15, no memory operand. */
    memcpy(bad, good, sizeof good);
    bad[0] = site_at(S6 + 10, 4, ISA_EXTRQ, ISA_MODE_INT3, 0, 1);
    CHECK(parses(bad, 1));
    bad[0].mem_slot = ISA_MEM_SRC;
    CHECK(!parses(bad, 1));
    bad[0] = site_at(S6 + 10, 4, ISA_INSERTQ, ISA_MODE_INT3, 0,
                     ISA_NO_REG); /* INSERTQ always has a source */
    CHECK(!parses(bad, 1));
}

/* tests/data/isa_table_fixture.hex: the table scripts/isa_rewrite.py writes for the synthetic
 * program of tests/test_isa_rewrite.py. '#' lines are comments; the rest is hex text. */
static unsigned char* read_hex_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "r");
    CHECK(f != NULL);
    unsigned char* data = malloc(4096);
    char line[256];
    *size = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        for (const char* p = line; p[0] && p[1] && p[0] != '\n'; p += 2) {
            unsigned value;
            CHECK(sscanf(p, "%2x", &value) == 1);
            data[(*size)++] = (unsigned char)value;
        }
    }
    fclose(f);
    return data;
}

static void test_table_written_by_the_generator(void) {
    size_t size;
    unsigned char* data = read_hex_file("tests/data/isa_table_fixture.hex", &size);
    IsaTable* t = isa_table_parse(data, size);
    free(data);
    CHECK(t != NULL && t->count == 8 && t->unsupported == 2 && t->flags == 0);
    CHECK(t->image_size == 0x1000 + 0x110);
    static const struct {
        uint64_t offset;
        uint8_t length, kind, mode;
    } expected[8] = {
        {0x1000, 4, ISA_MOVBE_LOAD, ISA_MODE_JMP_RELOCATE},
        {0x1006, 5, ISA_BLSR, ISA_MODE_JMP},
        {0x1010, 4, ISA_LZCNT, ISA_MODE_INT3},
        {0x1030, 5, ISA_MOVBE_LOAD, ISA_MODE_UNSUPPORTED},
        {0x1040, 4, ISA_EXTRQ, ISA_MODE_JMP_RELOCATE},
        {0x1050, 6, ISA_INSERTQ, ISA_MODE_JMP},
        {0x1060, 5, ISA_VCVTPH2PS, ISA_MODE_UNSUPPORTED},
        {0x1070, 8, ISA_MOVBE_LOAD, ISA_MODE_JMP},
    };
    for (uint32_t i = 0; i < 8; ++i)
        CHECK(t->sites[i].offset == expected[i].offset &&
              t->sites[i].length == expected[i].length && t->sites[i].kind == expected[i].kind &&
              t->sites[i].mode == expected[i].mode);
    CHECK(t->sites[0].relocated_length == 2 && t->sites[0].relocated[0] == 0x89 &&
          t->sites[0].relocated[1] == 0xc8);
    CHECK(t->sites[0].mem_slot == ISA_MEM_SRC && t->sites[0].base == 0 && t->sites[0].dst == 1);
    CHECK((t->sites[5].mem_flags & ISA_FLAG_IMM) && t->sites[5].disp == 0x0810 &&
          t->sites[5].dst == 0 && t->sites[5].src1 == 1);
    CHECK((t->sites[7].mem_flags & ISA_MEM_RIP_RELATIVE) && t->sites[7].disp == 0x1070 + 8 + 0x10);
    CHECK(t->sites[4].dst == 0 && t->sites[4].src1 == 1); /* extrq xmm0, xmm1 */
    isa_table_free(t);
}

static void test_sha256_match(void) {
    /* FIPS 180-4 vectors: "abc" and the empty message. */
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    static const uint8_t empty[32] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
                                      0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
                                      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
                                      0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    IsaTable t;
    memset(&t, 0, sizeof t);
    t.image_size = 3;
    memcpy(t.sha256, abc, 32);
    CHECK(isa_table_matches(&t, (const unsigned char*)"abc", 3) == 0);
    CHECK(isa_table_matches(&t, (const unsigned char*)"abd", 3) != 0);
    CHECK(isa_table_matches(&t, (const unsigned char*)"abc", 2) != 0); /* size differs */
    t.image_size = 0;
    memcpy(t.sha256, empty, 32);
    CHECK(isa_table_matches(&t, (const unsigned char*)"", 0) == 0);
    /* FIPS 180-4's 448-bit message: 56 bytes, so the padding spills into a second block. */
    static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t d448[32] = {0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
                                     0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
                                     0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
                                     0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
    t.image_size = sizeof m448 - 1;
    memcpy(t.sha256, d448, 32);
    CHECK(t.image_size == 56 && isa_table_matches(&t, (const unsigned char*)m448, 56) == 0);
}

int main(void) {
    region = mmap(NULL, 5 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    CHECK(region != MAP_FAILED);
    image = region;
    stubs = region + PAGE;
    memset(pristine, 0x90, PAGE);
    memcpy(pristine + S1, S1_CODE, sizeof S1_CODE);
    memcpy(pristine + S2, S2_CODE, sizeof S2_CODE);
    memcpy(pristine + S3, S3_CODE, sizeof S3_CODE);
    memcpy(pristine + S4, S4_CODE, sizeof S4_CODE);
    memcpy(pristine + S5, S5_CODE, sizeof S5_CODE);
    memcpy(pristine + S6, S6_CODE, sizeof S6_CODE);
    memcpy(pristine + S7, S7_CODE, sizeof S7_CODE);
    memcpy(pristine + S8, S8_CODE, sizeof S8_CODE);
    memcpy(pristine + S9, S6_CODE, sizeof S6_CODE); /* S9 = S6, trapped instead of jumped */
    memcpy(pristine + S10, S10_CODE, sizeof S10_CODE);
    isa_emulate_set_image(image);

    test_table_validation();
    test_table_written_by_the_generator();
    test_sha256_match();
    test_jmp_and_flags();
    test_relocated_site();
    test_adjacent_and_long_relocation();
    test_sse4a_patched();
    test_stub_keeps_red_zone();
    test_int3_sites();
    test_changed_bytes_are_skipped();
    test_changed_next_instruction_downgrades();
    test_unsupported_needed_writes_nothing();
    test_out_of_rel32_range_is_skipped();
    test_stub_lookup_and_report();
    puts("PASS: ISA rewrite (table validation, stubs run patched code, SSE4a via stub and via "
         "SIGTRAP, patch modes)");
    return 0;
}
