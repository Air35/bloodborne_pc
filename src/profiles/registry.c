// SPDX-License-Identifier: GPL-2.0-or-later
/* Registry and application of title profiles (ADR-0016), public API in title_profile.h. Layer
 * L6: depends only on the public runtime API (runtime.h) and on the profile files; the runtime
 * does not depend on this. It lives in profiles/ because it is the list of games. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profiles/profiles.h"
#include "runtime.h"
#include "title_profile.h"

/* Add a title: one file in src/profiles/ and one line here. */
static const TitleProfile* const registry[] = {
    &title_profile_sotc,
    &title_profile_bloodborne,
};

const TitleProfile* title_profile_find(const char* title_id) {
    if (!title_id || !*title_id)
        return NULL;
    for (size_t i = 0; i < sizeof registry / sizeof *registry; ++i)
        for (const char* const* id = registry[i]->title_ids; *id; ++id)
            if (!strcmp(*id, title_id))
                return registry[i];
    return NULL;
}

const TitleProfile* title_profile_apply(const char* title_id, const char* save_id,
                                        const char* user_dir) {
    const char* forced = getenv("SOTC_TITLE_PROFILE");
    const TitleProfile* profile =
        forced && !strcmp(forced, "none") ? NULL : title_profile_find(title_id);
    runtime_set_neo_mode(profile ? profile->neo_mode : 0);
    if (!profile) {
        printf("Runtime: no profile for title %s: neutral core defaults\n",
               title_id && *title_id ? title_id : "(unknown)");
        return NULL;
    }
    printf("Runtime: profile '%s' for title %s\n", profile->name, title_id);
    if (profile->on_start) {
        const TitleEnv env = {title_id, save_id && *save_id ? save_id : title_id, user_dir};
        profile->on_start(&env);
    }
    return profile;
}
