// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: texture LOD statistics (PM4 GET_LOD_STATS). A T# with LOD_HW_CNT_EN counts, in its
// counter bank (COUNTER_BANK_ID, 256 banks), how its texture was sampled; GET_LOD_STATS writes
// the banks to memory. Shadow of the Colossus streams the fine mips of its textures from these
// numbers: without them it never loads more than the 256x256 mip (blurry ground and rocks,
// black water, missing waterfall sheets). The port marks the banks of the T#s bound by draws
// and dispatches; GET_LOD_STATS reports a bank marked in the last reports as sampled (the
// hardware accumulates over many frames too: a texture missing from one frame must not read as
// unused, or the game drops mips it is still loading).
// The hardware knows the LOD each texture is sampled at; the port does not, so every sampled
// bank asks for the same mip. Asking for mip 0 everywhere fills the game's streaming pool and
// the game then breaks guest memory (heap and descriptor corruption within 1-4 minutes of the
// attract demo, also with mip 1); mip 2 (512x512 for the 2048 textures, 4x the old 256x256)
// ran the whole demo. SOTC_LOD_LEVEL picks another mip for tests.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include "common/types.h"

namespace SotcLod {

inline std::atomic<u32> uses[256]{};
/// Reports a bank stays sampled after its last use (about 3 s at 60 reports per second).
constexpr u32 Linger = 180;

inline void Mark(u32 bank) {
    uses[bank & 0xff].fetch_add(1, std::memory_order_relaxed);
}

/// The 256 bank entries the game reads (u64 each): bits 56-59 the finest mip sampled (15: not
/// sampled), bits 32-55 how much (the game's streaming priority). Called once per report.
inline void Collect(u64 (&entries)[256]) {
    // SOTC_LOD_LINGER=<reports>: how long a bank stays sampled (test knob).
    static const u32 linger = [] {
        const char* v = std::getenv("SOTC_LOD_LINGER");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : Linger;
    }();
    // SOTC_LOD_LEVEL=<mip>: the finest mip reported for a sampled bank (default 2, see above).
    static const u64 level = [] {
        const char* v = std::getenv("SOTC_LOD_LEVEL");
        return v ? std::strtoull(v, nullptr, 0) & 0xf : 2;
    }();
    static u32 idle[256] = {[0 ... 255] = 0xffffffffu};
    static u32 last_count[256]{};
    for (u32 i = 0; i < 256; ++i) {
        const u32 n = uses[i].exchange(0, std::memory_order_relaxed);
        if (n) {
            idle[i] = 0;
            last_count[i] = std::min<u32>(n * 256u, 0xffffff);
        } else if (idle[i] < linger) {
            ++idle[i];
        }
        entries[i] = idle[i] < linger ? (level << 56) | (u64{std::max<u32>(last_count[i], 1)} << 32)
                                      : u64{0xf} << 56;
    }
}

} // namespace SotcLod
