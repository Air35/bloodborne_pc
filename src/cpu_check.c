// SPDX-License-Identifier: GPL-2.0-or-later
/* See cpu_check.h. Adapted from gow3 91b6e61 src/cpu_check.c (set tables and the missing-set
 * walk); OSXSAVE/XCR0, SSE4a and the policy are new. */
#include "cpu_check.h"

#include <cpuid.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { LEAF1, LEAF7, EXT1 };
enum {
    L1_SSE42 = 1u << 20,
    L1_MOVBE = 1u << 22,
    L1_POPCNT = 1u << 23,
    L1_OSXSAVE = 1u << 27,
    L1_AVX = 1u << 28,
    L1_F16C = 1u << 29,
    L7_BMI1 = 1u << 3,
    E1_LZCNT = 1u << 5,
    E1_SSE4A = 1u << 6,
    XCR0_SSE_AVX_STATE = 6u,
};

CpuFeatureWords cpu_feature_words(void) {
    CpuFeatureWords words = {0, 0, 0, 0};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d))
        words.leaf1_ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
        words.leaf7_ebx = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d))
        words.ext1_ecx = c;
    if (words.leaf1_ecx & L1_OSXSAVE) {
        unsigned lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        words.xcr0 = lo;
    }
    return words;
}

/* A set the guest may use. `word` picks the CPUID word, `bit` the feature bit. */
typedef struct {
    const char* name;
    int word;
    uint32_t bit;
    unsigned set;
} CpuSet;

static const CpuSet fatal_sets[] = {
    {"SSE4.2", LEAF1, L1_SSE42, 0},
    {"POPCNT", LEAF1, L1_POPCNT, 0},
};
/* Display order of cpu_set_names is the order of this table (as gow3 91b6e61, SSE4a at the end). */
static const CpuSet rewritable_sets[] = {
    {"MOVBE", LEAF1, L1_MOVBE, CPU_SET_MOVBE}, {"F16C", LEAF1, L1_F16C, CPU_SET_F16C},
    {"BMI1", LEAF7, L7_BMI1, CPU_SET_BMI1},    {"LZCNT", EXT1, E1_LZCNT, CPU_SET_LZCNT},
    {"SSE4a", EXT1, E1_SSE4A, CPU_SET_SSE4A},
};

static int have(CpuFeatureWords words, const CpuSet* set) {
    const uint32_t values[] = {words.leaf1_ecx, words.leaf7_ebx, words.ext1_ecx};
    return (values[set->word] & set->bit) != 0;
}

/* AVX is usable only if the CPU has it, the OS enabled OSXSAVE, and XCR0 saves SSE and AVX state:
 * the bit alone is not enough (a hypervisor or kernel can mask it out). */
static int avx_usable(CpuFeatureWords words) {
    return (words.leaf1_ecx & L1_AVX) && (words.leaf1_ecx & L1_OSXSAVE) &&
           (words.xcr0 & XCR0_SSE_AVX_STATE) == XCR0_SSE_AVX_STATE;
}

/* Appends ", name" (or "name") to out, cut at size; keeps the count in *count. */
static void append_name(char* out, size_t size, size_t* used, unsigned* count, const char* name) {
    if (*used < size) {
        const int n = snprintf(out + *used, size - *used, "%s%s", *count ? ", " : "", name);
        *used = n < 0 ? size : *used + (size_t)n;
        if (*used >= size)
            *used = size - 1;
    }
    ++*count;
}

int cpu_missing_fatal(CpuFeatureWords words, char* out, size_t size) {
    size_t used = 0;
    unsigned count = 0;
    if (size)
        out[0] = '\0';
    for (size_t i = 0; i < sizeof fatal_sets / sizeof *fatal_sets; ++i)
        if (!have(words, &fatal_sets[i]))
            append_name(out, size, &used, &count, fatal_sets[i].name);
    if (!avx_usable(words))
        append_name(out, size, &used, &count, "AVX");
    return (int)count;
}

unsigned cpu_missing_rewritable(CpuFeatureWords words, char* out, size_t size) {
    unsigned mask = 0;
    for (size_t i = 0; i < sizeof rewritable_sets / sizeof *rewritable_sets; ++i)
        if (!have(words, &rewritable_sets[i]))
            mask |= rewritable_sets[i].set;
    cpu_set_names(mask, out, size);
    return mask;
}

unsigned cpu_set_names(unsigned mask, char* out, size_t size) {
    size_t used = 0;
    unsigned count = 0;
    if (size)
        out[0] = '\0';
    for (size_t i = 0; i < sizeof rewritable_sets / sizeof *rewritable_sets; ++i)
        if (mask & rewritable_sets[i].set)
            append_name(out, size, &used, &count, rewritable_sets[i].name);
    return count;
}

/* ---- policy ---- */

/* One set name, case-insensitive; 0 when unknown. */
static unsigned set_by_name(const char* name, size_t length) {
    for (size_t i = 0; i < sizeof rewritable_sets / sizeof *rewritable_sets; ++i) {
        const char* candidate = rewritable_sets[i].name;
        if (strlen(candidate) != length)
            continue;
        size_t k = 0;
        while (k < length &&
               tolower((unsigned char)candidate[k]) == tolower((unsigned char)name[k]))
            ++k;
        if (k == length)
            return rewritable_sets[i].set;
    }
    return 0;
}

/* "a,b,c" into a mask; 0 for an empty list, an empty item or an unknown name. */
static unsigned parse_list(const char* list) {
    unsigned mask = 0;
    const char* item = list;
    for (;;) {
        const char* end = strchr(item, ',');
        const size_t length = end ? (size_t)(end - item) : strlen(item);
        const unsigned set = length ? set_by_name(item, length) : 0;
        if (!set)
            return 0;
        mask |= set;
        if (!end)
            return mask;
        item = end + 1;
    }
}

int isa_policy_parse(const char* text, IsaPolicy* out) {
    out->mode = ISA_POLICY_AUTO;
    out->listed = 0;
    if (!text || !*text || !strcmp(text, "auto"))
        return 0;
    if (!strcmp(text, "native")) {
        out->mode = ISA_POLICY_NATIVE;
        return 0;
    }
    if (!strcmp(text, "rewrite")) {
        out->mode = ISA_POLICY_REWRITE;
        out->listed = CPU_SET_ALL;
        return 0;
    }
    if (!strncmp(text, "rewrite:", 8)) {
        out->listed = parse_list(text + 8);
        if (!out->listed)
            return -1;
        out->mode = ISA_POLICY_REWRITE;
        return 0;
    }
    return -1;
}

int isa_policy_resolve(const IsaPolicy* policy, unsigned missing, unsigned* sets) {
    *sets = missing;
    if (policy->mode == ISA_POLICY_NATIVE)
        return missing ? -1 : 0;
    if (policy->mode == ISA_POLICY_REWRITE)
        *sets |= policy->listed;
    return 0;
}
