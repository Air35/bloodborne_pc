// SPDX-License-Identifier: GPL-2.0-or-later
/* Profile of Shadow of the Colossus (PS4, 2018), edition CUSA08809 01.00 + update 01.01
 * (ADR-0003). Only this edition is declared: CUSA08034 and CUSA08804 stay out until there is a
 * dump of the maintainer's own to test them. The base PS4 mode (ADR-0009) is the core's neutral
 * default, so there is nothing to push yet; patches by signature, the 60 fps option and the
 * DLC profile come with phases A5 and A10 and belong here, not in the core. */
#include "profiles/profiles.h"

#include "runtime.h"

static const char* const title_ids[] = {"CUSA08809", NULL};

/* BPE heap guard (ShadowOfTheColossus-PC 70bb438 src/probe.c, from Pink-shadPS4): GPU writes of
 * dispatches on stale descriptors land in freed heap blocks and readbacks then overwrite the
 * allocator's metadata (access violations in eboot+0x48xxx/0x49xxx). The heap object is the
 * first argument of the heap's free function at eboot+0x495e0; its prologue is checked first. */
static void on_start(const TitleEnv* env) {
    (void)env;
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    runtime_set_heap_probe(0x495e0, prologue, sizeof prologue);
}

const TitleProfile title_profile_sotc = {
    .name = "sotc",
    .title_ids = title_ids,
    .neo_mode = 0, /* ADR-0009 */
    .on_start = on_start,
};
