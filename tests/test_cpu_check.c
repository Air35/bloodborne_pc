// SPDX-License-Identifier: GPL-2.0-or-later
/* Host CPU classification and ISA policy (src/cpu_check.c, docs/10 §2-§3, ADR-0010).
 * Model: gow3 91b6e61 tests/test_cpu_check.c, extended with OSXSAVE/XCR0, SSE4a and the policy.
 * CPUID words are simulated, so the result does not depend on the machine running the test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cpu_check.h"

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
    XCR0_AVX_STATE = 6u, /* x87-adjacent SSE and AVX state enabled by the OS */
};
static const uint32_t BASE = L1_SSE42 | L1_POPCNT | L1_AVX | L1_OSXSAVE;

static CpuFeatureWords words(uint32_t l1, uint32_t l7, uint32_t e1, uint32_t xcr0) {
    return (CpuFeatureWords){l1, l7, e1, xcr0};
}

static void classification(void) {
    char out[128];
    /* Zen: everything the Jaguar has, SSE4a included. */
    CpuFeatureWords zen =
        words(BASE | L1_MOVBE | L1_F16C, L7_BMI1, E1_LZCNT | E1_SSE4A, XCR0_AVX_STATE | 1);
    assert(cpu_missing_fatal(zen, out, sizeof out) == 0 && !strcmp(out, ""));
    assert(cpu_missing_rewritable(zen, out, sizeof out) == 0 && !strcmp(out, ""));
    /* Haswell (Intel): all but SSE4a, which no Intel CPU has. */
    CpuFeatureWords haswell =
        words(BASE | L1_MOVBE | L1_F16C, L7_BMI1, E1_LZCNT, XCR0_AVX_STATE | 1);
    assert(cpu_missing_fatal(haswell, out, sizeof out) == 0);
    assert(cpu_missing_rewritable(haswell, out, sizeof out) == CPU_SET_SSE4A &&
           !strcmp(out, "SSE4a"));
    /* Ivy Bridge, which is what the development VM exposes: F16C, but no MOVBE/BMI1/LZCNT/SSE4a. */
    CpuFeatureWords ivy = words(BASE | L1_F16C, 0, 0, XCR0_AVX_STATE | 1);
    assert(cpu_missing_fatal(ivy, out, sizeof out) == 0);
    assert(cpu_missing_rewritable(ivy, out, sizeof out) ==
           (CPU_SET_MOVBE | CPU_SET_BMI1 | CPU_SET_LZCNT | CPU_SET_SSE4A));
    assert(!strcmp(out, "MOVBE, BMI1, LZCNT, SSE4a"));
    /* Sandy Bridge: no F16C either. */
    CpuFeatureWords sandy = words(BASE, 0, 0, XCR0_AVX_STATE | 1);
    assert(cpu_missing_rewritable(sandy, out, sizeof out) ==
           (CPU_SET_MOVBE | CPU_SET_F16C | CPU_SET_BMI1 | CPU_SET_LZCNT | CPU_SET_SSE4A));
    assert(!strcmp(out, "MOVBE, F16C, BMI1, LZCNT, SSE4a"));
}

static void avx_needs_the_os(void) {
    char out[128];
    /* AVX bit without OSXSAVE: the CPU has it, the OS did not turn it on. */
    CpuFeatureWords no_osxsave = words((BASE & ~L1_OSXSAVE), 0, 0, 0);
    assert(cpu_missing_fatal(no_osxsave, out, sizeof out) == 1 && !strcmp(out, "AVX"));
    /* OSXSAVE set but XCR0 does not save the AVX state. */
    CpuFeatureWords no_state = words(BASE, 0, 0, 1);
    assert(cpu_missing_fatal(no_state, out, sizeof out) == 1 && !strcmp(out, "AVX"));
    /* Nehalem: SSE4.2 and POPCNT but no AVX. */
    CpuFeatureWords nehalem = words(L1_SSE42 | L1_POPCNT, 0, 0, 0);
    assert(cpu_missing_fatal(nehalem, out, sizeof out) == 1 && !strcmp(out, "AVX"));
    /* Nothing at all: three fatal sets, and a short buffer is cut, never overrun. */
    memset(out, 'x', sizeof out);
    assert(cpu_missing_fatal(words(0, 0, 0, 0), out, 8) == 3 && strlen(out) < 8);
}

static void names(void) {
    char out[64];
    assert(cpu_set_names(0, out, sizeof out) == 0 && !strcmp(out, ""));
    assert(cpu_set_names(CPU_SET_SSE4A | CPU_SET_MOVBE, out, sizeof out) == 2 &&
           !strcmp(out, "MOVBE, SSE4a"));
    assert(cpu_set_names(CPU_SET_ALL, out, sizeof out) == 5);
}

static void policy(void) {
    IsaPolicy p;
    unsigned sets = 0xdead;
    const unsigned ivy = CPU_SET_MOVBE | CPU_SET_BMI1 | CPU_SET_LZCNT | CPU_SET_SSE4A;
    /* Text: default and valid values. */
    assert(isa_policy_parse(NULL, &p) == 0 && p.mode == ISA_POLICY_AUTO);
    assert(isa_policy_parse("", &p) == 0 && p.mode == ISA_POLICY_AUTO);
    assert(isa_policy_parse("auto", &p) == 0 && p.mode == ISA_POLICY_AUTO);
    assert(isa_policy_parse("native", &p) == 0 && p.mode == ISA_POLICY_NATIVE);
    assert(isa_policy_parse("rewrite", &p) == 0 && p.mode == ISA_POLICY_REWRITE &&
           p.listed == CPU_SET_ALL);
    assert(isa_policy_parse("rewrite:sse4a", &p) == 0 && p.mode == ISA_POLICY_REWRITE &&
           p.listed == CPU_SET_SSE4A);
    assert(isa_policy_parse("rewrite:movbe,lzcnt", &p) == 0 &&
           p.listed == (CPU_SET_MOVBE | CPU_SET_LZCNT));
    assert(isa_policy_parse("rewrite:SSE4A", &p) == 0 &&
           p.listed == CPU_SET_SSE4A); /* case-insensitive */
    /* Text: invalid values are errors, never silently "auto". */
    assert(isa_policy_parse("rewrite:", &p) == -1);
    assert(isa_policy_parse("rewrite:avx", &p) == -1);
    assert(isa_policy_parse("fast", &p) == -1);
    assert(isa_policy_parse("rewrite:sse4a,", &p) == -1);

    /* Resolution: what gets rewritten for each mode and host. */
    isa_policy_parse("auto", &p);
    assert(isa_policy_resolve(&p, 0, &sets) == 0 && sets == 0);
    assert(isa_policy_resolve(&p, ivy, &sets) == 0 && sets == ivy);
    isa_policy_parse("native", &p);
    assert(isa_policy_resolve(&p, 0, &sets) == 0 && sets == 0);
    assert(isa_policy_resolve(&p, ivy, &sets) == -1); /* would die with SIGILL: stop instead */
    isa_policy_parse("rewrite", &p);
    assert(isa_policy_resolve(&p, 0, &sets) == 0 && sets == CPU_SET_ALL); /* differential test */
    isa_policy_parse("rewrite:sse4a", &p);
    assert(isa_policy_resolve(&p, 0, &sets) == 0 && sets == CPU_SET_SSE4A); /* isolates one set */
    /* A set the host lacks is always rewritten, listed or not: never leave a SIGILL behind. */
    assert(isa_policy_resolve(&p, ivy, &sets) == 0 && sets == ivy);
}

int main(void) {
    classification();
    avx_needs_the_os();
    names();
    policy();
    /* The words of the machine running the test: only a consistency check, no assumption on it. */
    char out[128];
    CpuFeatureWords here = cpu_feature_words();
    const int fatal = cpu_missing_fatal(here, out, sizeof out);
    const unsigned missing = cpu_missing_rewritable(here, out, sizeof out);
    printf("host: %d fatal sets missing, rewritable mask %#x (%s)\n", fatal, missing,
           out[0] ? out : "none");
    puts("PASS: CPU classification (Zen, Haswell, Ivy, Sandy, no OS AVX, Nehalem), set names, ISA "
         "policy");
    return 0;
}
