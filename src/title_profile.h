// SPDX-License-Identifier: GPL-2.0-or-later
/* Per-title knowledge (layer L6, ADR-0016, docs/05 §23).
 *
 * The core (loader, runtime, GPU, scripts) does not know which game runs. The loader asks this
 * module once, with the param.sfo TITLE_ID, and the profile *pushes* its settings into the
 * runtime (runtime_set_*) and runs its start hook. A title with no profile is a valid case: the
 * core keeps its neutral defaults. One profile per title lives in src/profiles/<title>.c; the
 * GPU has the same idea in gpu/games/ (Game::Profile). */
#ifndef SOTC_TITLE_PROFILE_H
#define SOTC_TITLE_PROFILE_H

#include <stddef.h> /* NULL: profiles end their id lists with it (include-what-you-use) */

typedef struct TitleEnv {
    const char* title_id; /* param.sfo TITLE_ID that selected the profile */
    const char* save_id;  /* save data directory id (INSTALL_DIR_SAVEDATA, else the TITLE_ID) */
    const char* user_dir; /* the runtime's per-user data root */
} TitleEnv;

typedef struct TitleProfile {
    const char* name;                      /* for the log only */
    const char* const* title_ids;          /* NULL-terminated param.sfo TITLE_IDs */
    int neo_mode;                          /* sceKernelIsNeoMode result for this title */
    void (*on_start)(const TitleEnv* env); /* NULL: nothing; runs before the guest starts */
} TitleProfile;

/* The profile that lists title_id, or NULL (unknown title, NULL or empty id). */
const TitleProfile* title_profile_find(const char* title_id);

/* Selects the profile for title_id and applies it to the runtime: neutral defaults first, then
 * the profile's values and start hook. Returns the profile, or NULL for the neutral core.
 * SOTC_TITLE_PROFILE=none forces the neutral core (checks the generic paths). */
const TitleProfile* title_profile_apply(const char* title_id, const char* save_id,
                                        const char* user_dir);

#endif
