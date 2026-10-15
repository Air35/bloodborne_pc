// SPDX-License-Identifier: GPL-2.0-or-later
/* Title profiles (src/profiles/registry.c, ADR-0016). This is the only test that
 * names real TITLE_IDs: the core's tests use fictitious ones. Everything is synthetic: a save
 * file of zeros in a temporary directory stands in for the game's. */
#define _GNU_SOURCE
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "runtime.h"
#include "title_profile.h"

void runtime_restart(void) {
    abort();
}

enum { FLAG_OFFSET = 0x204E, FILE_SIZE = 0x2050 };

static int remove_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static int neo_mode(void) {
    typedef int32_t(ABI * NoArgs)(void);
    NoArgs is_neo = (NoArgs)runtime_resolve("WslcK1FQcGI#libkernel", 0);
    assert(is_neo);
    return is_neo();
}

/* <root>/savedata/1/<id>/SPRJ0005/userdata0010 of FILE_SIZE zeros; returns its path. */
static void make_save(const char* root, const char* id, char* path, size_t size) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/savedata", root);
    (void)mkdir(dir, 0755); /* parents may exist from a previous call */
    snprintf(dir, sizeof(dir), "%s/savedata/1", root);
    (void)mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/savedata/1/%s", root, id);
    (void)mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/savedata/1/%s/SPRJ0005", root, id);
    (void)mkdir(dir, 0755);
    snprintf(path, size, "%s/userdata0010", dir);
    FILE* f = fopen(path, "wb");
    assert(f);
    unsigned char zeros[FILE_SIZE] = {0};
    assert(fwrite(zeros, 1, sizeof(zeros), f) == sizeof(zeros) && !fclose(f));
}

static int flag_of(const char* path) {
    FILE* f = fopen(path, "rb");
    assert(f && !fseek(f, FLAG_OFFSET, SEEK_SET));
    const int value = fgetc(f);
    fclose(f);
    return value;
}

int main(void) {
    runtime_start(1);

    /* Lookup: known titles, unknown and invalid ids. */
    const TitleProfile* sotc = title_profile_find("CUSA08809");
    assert(sotc && !strcmp(sotc->name, "sotc") && sotc->neo_mode == 0);
    const TitleProfile* bloodborne = title_profile_find("CUSA00207");
    assert(bloodborne && bloodborne->on_start && !strcmp(bloodborne->name, "bloodborne"));
    assert(!title_profile_find("TEST00000") && !title_profile_find("") &&
           !title_profile_find(NULL));
    assert(!title_profile_find("CUSA08034")); /* other SotC editions are not declared (ADR-0003) */

    /* An unknown title is the neutral core: defaults, no hook, no file touched. */
    char root[] = "/tmp/title-profile-test-XXXXXX";
    assert(mkdtemp(root));
    char save[700];
    make_save(root, "TEST00000", save, sizeof(save));
    assert(neo_mode() == 0);
    runtime_set_neo_mode(1); /* a leftover from another title must not survive */
    assert(neo_mode() == 1);
    assert(title_profile_apply("TEST00000", "TEST00000", root) == NULL && neo_mode() == 0);
    assert(flag_of(save) == 0);

    /* The Bloodborne profile's start hook sets the flag in its own save, once per run. */
    char bb_save[700];
    make_save(root, "CUSA00207", bb_save, sizeof(bb_save));
    assert(flag_of(bb_save) == 0);
    assert(title_profile_apply("CUSA00207", "CUSA00207", root) == bloodborne);
    assert(flag_of(bb_save) == 1 && flag_of(save) == 0);
    assert(title_profile_apply("CUSA00207", "CUSA00207", root) == bloodborne &&
           flag_of(bb_save) == 1);

    /* The profile's own switch and the forced neutral core leave the save alone. */
    make_save(root, "CUSA00207", bb_save, sizeof(bb_save)); /* zeros again */
    setenv("BB_SOUND_HACK", "0", 1);
    assert(title_profile_apply("CUSA00207", "CUSA00207", root) == bloodborne &&
           flag_of(bb_save) == 0);
    unsetenv("BB_SOUND_HACK");
    setenv("SOTC_TITLE_PROFILE", "none", 1);
    assert(title_profile_apply("CUSA00207", "CUSA00207", root) == NULL && flag_of(bb_save) == 0);
    unsetenv("SOTC_TITLE_PROFILE");

    /* The SotC profile has no hook and keeps the base PS4 mode of ADR-0009. */
    assert(title_profile_apply("CUSA08809", "CUSA08809", root) == sotc && neo_mode() == 0);

    assert(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("PASS: title profiles (lookup, neutral core, start hook, switches)");
    return 0;
}
