// SPDX-License-Identifier: GPL-2.0-or-later
/* Profile of Bloodborne (bbport lineage). Kept as a profile, not deleted (ADR-0016): it is
 * *not declared supported* by this project (ADR-0003). The only thing here is the "sound hack"
 * that lived inside runtime_savedata.c.
 *
 * Origin: bbport 3c57da4 src/runtime_savedata.c (rainvmaker, from the Diegolix29 shadPS4 fork):
 * the game data save carries a flag at 0x204E of userdata0010; with it clear, parts of the
 * game's audio (e.g. the player's weapon sounds) never play. It is set before the game reads the
 * save. BB_SOUND_HACK=0 leaves saves untouched (variable kept until the sotc rename). */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profiles/profiles.h"

enum { FLAG_OFFSET = 0x204E };

static const char* const title_ids[] = {"CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900",
                                        "CUSA01363", "CUSA03014", "CUSA03023", "CUSA03173",
                                        "CUSA03179", NULL};

/* Sets the flag in <user>/savedata/<user id>/<save id>/SPRJ0005/userdata0010 of every user. */
static void sound_hack(const TitleEnv* env) {
    const char* disabled = getenv("BB_SOUND_HACK");
    if (disabled && disabled[0] == '0')
        return;
    char users[700];
    snprintf(users, sizeof(users), "%s/savedata", env->user_dir);
    DIR* dir = opendir(users);
    if (!dir)
        return;
    for (struct dirent* e; (e = readdir(dir));) {
        if (e->d_name[0] == '.')
            continue;
        char path[1100];
        snprintf(path, sizeof(path), "%s/%s/%s/SPRJ0005/userdata0010", users, e->d_name,
                 env->save_id);
        FILE* f = fopen(path, "r+b");
        if (!f)
            continue;
        const int old = fseek(f, FLAG_OFFSET, SEEK_SET) ? EOF : fgetc(f);
        if (old != EOF && old != 1 && !fseek(f, FLAG_OFFSET, SEEK_SET)) {
            fputc(1, f);
            printf("Runtime: sound flag set in %s (was %d)\n", path, old);
        }
        fclose(f);
    }
    closedir(dir);
}

const TitleProfile title_profile_bloodborne = {
    .name = "bloodborne",
    .title_ids = title_ids,
    .neo_mode = 0, /* neutral: whether this title wants another value is [a verificar] */
    .on_start = sound_hack,
};
