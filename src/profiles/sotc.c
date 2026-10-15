// SPDX-License-Identifier: GPL-2.0-or-later
/* Profile of Shadow of the Colossus (PS4, 2018), edition CUSA08809 01.00 + update 01.01
 * (ADR-0003). Only this edition is declared: CUSA08034 and CUSA08804 stay out until there is a
 * dump of the maintainer's own to test them. The base PS4 mode (ADR-0009) is the core's neutral
 * default, so there is nothing to push yet; patches by signature, the 60 fps option and the
 * DLC profile come with phases A5 and A10 and belong here, not in the core. */
#include "profiles/profiles.h"

static const char* const title_ids[] = {"CUSA08809", NULL};

const TitleProfile title_profile_sotc = {
    .name = "sotc",
    .title_ids = title_ids,
    .neo_mode = 0, /* ADR-0009 */
    .on_start = NULL,
};
