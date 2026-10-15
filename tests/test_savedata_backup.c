// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: save data backup (runtime_savedata.c, plan docs/plans/2026-10-15-a3-hle-lote5.md):
 * sceSaveDataUmountWithBackup, CheckBackupData, RestoreBackupData, GetMountInfo. Contracts:
 * shadPS4 b4e7ae7 save_data/savedata.cpp:587-640, 995-1017, 1322-1360, 1715 (consulted). Only
 * synthetic saves in a temporary directory; the call shapes mirror the SotC sites at
 * eboot+0xa48db9 (UmountWithBackup), 0xa49da2 (CheckBackupData, param out), 0xa48f72
 * (RestoreBackupData) and 0xa4aa93 (GetMountInfo, uses `blocks` << 15). */
#define _GNU_SOURCE
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "runtime.h"

void runtime_restart(void) {
    abort();
}

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
enum { MODE_CREATE = 4, USER = 1, BLOCK = 32768 };

typedef struct {
    char data[32];
} DirName;
typedef struct {
    char data[16];
} MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct {
    int32_t user, pad;
    const DirName* dir;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
    int32_t pad2;
} Mount2;
typedef struct {
    MountPoint point;
    uint64_t required_blocks;
    uint32_t unused, status;
    uint8_t reserved[28];
    int32_t pad;
} MountResult;
typedef struct {
    int32_t user, pad;
    const void* title;
    const DirName* dir;
    Param* param;
    void* icon;
    uint8_t reserved[32];
} CheckBackup;
typedef struct {
    int32_t user, pad;
    const void* title;
    const DirName* dir;
    const void* fingerprint;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad2;
} RestoreBackup;
typedef struct {
    uint64_t blocks, free_blocks;
    uint8_t reserved[32];
} MountInfo;
_Static_assert(sizeof(CheckBackup) == 72 && sizeof(RestoreBackup) == 72, "layouts of the SotC");

typedef int32_t(ABI* Init)(const void*);
typedef int32_t(ABI* Mount)(const Mount2*, MountResult*);
typedef int32_t(ABI* Umount)(const MountPoint*);
typedef int32_t(ABI* SetParam)(const MountPoint*, uint32_t, const void*, uint64_t);
typedef int32_t(ABI* Check)(const CheckBackup*);
typedef int32_t(ABI* Restore)(const RestoreBackup*);
typedef int32_t(ABI* GetInfo)(const MountPoint*, MountInfo*);
typedef struct {
    int32_t user, pad;
    const void* title;
    const DirName* dir;
    uint32_t key, order;
    uint8_t reserved[32];
} SearchCond;
typedef struct {
    uint32_t hits;
    int32_t pad;
    DirName* names;
    uint32_t names_capacity, set_count;
    void *params, *infos;
    uint8_t reserved[12];
    int32_t pad2;
} SearchResult;
_Static_assert(sizeof(SearchCond) == 64 && sizeof(SearchResult) == 56, "layouts of the module");
typedef int32_t(ABI* Search)(const SearchCond*, SearchResult*);

static void* resolve(const char* name) {
    char nid[12], key[64];
    assert(runtime_nid(name, nid));
    snprintf(key, sizeof(key), "%s#libSceSaveData", nid);
    return (void*)runtime_resolve(key, 0);
}

static char host_dir[600]; /* where the module keeps SAVE0000 for USER */

static int remove_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void write_file(const char* name, const char* text) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", host_dir, name);
    FILE* f = fopen(path, "wb");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}

static void expect_file(const char* name, const char* text) {
    char path[700], got[64] = {0};
    snprintf(path, sizeof(path), "%s/%s", host_dir, name);
    FILE* f = fopen(path, "rb");
    assert(f && fread(got, 1, sizeof(got) - 1, f) == strlen(text) && !strcmp(got, text));
    fclose(f);
}

int main(void) {
    char user_root[] = "/tmp/save-backup-test-XXXXXX";
    assert(mkdtemp(user_root));
    runtime_file_configure(user_root, user_root);
    runtime_start(1);
    Init init = (Init)resolve("sceSaveDataInitialize3");
    Mount mount = (Mount)resolve("sceSaveDataMount2");
    Umount umount = (Umount)resolve("sceSaveDataUmount");
    Umount umount_backup = (Umount)resolve("sceSaveDataUmountWithBackup");
    SetParam set_param = (SetParam)resolve("sceSaveDataSetParam");
    Check check = (Check)resolve("sceSaveDataCheckBackupData");
    Restore restore = (Restore)resolve("sceSaveDataRestoreBackupData");
    GetInfo get_info = (GetInfo)resolve("sceSaveDataGetMountInfo");
    assert(init && mount && umount && umount_backup && set_param && check && restore && get_info);

    DirName dir = {"SAVE0000"}, missing = {"NOSAVE"};
    CheckBackup check_args = {.user = USER, .dir = &dir};
    RestoreBackup restore_args = {.user = USER, .dir = &dir};
    assert(check(&check_args) == ERR_NOT_INITIALIZED &&
           restore(&restore_args) == ERR_NOT_INITIALIZED);
    assert(init(NULL) == 0);
    assert(check(NULL) == ERR_PARAMETER && restore(NULL) == ERR_PARAMETER);
    snprintf(host_dir, sizeof(host_dir), "%s/savedata/%d/UNKNOWN/SAVE0000", user_root, USER);

    /* Mount info: total from the mount request, free = total - used / block size. */
    MountResult result;
    Mount2 request = {.user = USER, .dir = &dir, .blocks = 64, .mode = MODE_CREATE};
    assert(mount(&request, &result) == 0);
    write_file("data.bin", "v1");
    Param param = {.title = "SOTC TEST"};
    assert(set_param(&result.point, 0, &param, sizeof(param)) == 0);
    MountInfo info = {0};
    assert(get_info(&result.point, &info) == 0 && info.blocks == 64 && info.free_blocks == 64);
    char big[BLOCK + 1];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    write_file("big.bin", big);
    write_file("big2.bin", big);
    assert(get_info(&result.point, &info) == 0 && info.free_blocks == 64 - 2);
    assert(get_info(NULL, &info) == ERR_PARAMETER &&
           get_info(&result.point, NULL) == ERR_PARAMETER);
    MountPoint unmounted = {"/savedata9"};
    assert(get_info(&unmounted, &info) == ERR_NOT_MOUNTED);

    /* A mounted save is refused first (the reference checks it before looking for a backup);
     * once unmounted without a backup there is nothing to check or restore. */
    assert(check(&check_args) == ERR_BAD_MOUNTED && restore(&restore_args) == ERR_BAD_MOUNTED);
    assert(umount(&result.point) == 0);
    assert(check(&check_args) == ERR_NOT_FOUND && restore(&restore_args) == ERR_NOT_FOUND);
    assert(umount_backup(&unmounted) == ERR_NOT_FOUND);
    assert(mount(&(Mount2){.user = USER, .dir = &dir, .blocks = 64}, &result) == 0);
    assert(umount_backup(&result.point) == 0); /* v1 + param backed up */
    assert(get_info(&result.point, &info) == ERR_NOT_MOUNTED);

    Param seen = {0};
    check_args.param = &seen;
    assert(check(&check_args) == 0 && !strcmp(seen.title, "SOTC TEST"));
    CheckBackup other = {.user = USER, .dir = &missing};
    assert(check(&other) == ERR_NOT_FOUND);

    /* A plain unmount keeps the backup; restore brings v1 back over the changed save. */
    assert(mount(&(Mount2){.user = USER, .dir = &dir, .blocks = 64}, &result) == 0);
    write_file("data.bin", "v2");
    assert(restore(&restore_args) == ERR_BAD_MOUNTED);
    assert(umount(&result.point) == 0);
    expect_file("data.bin", "v2");
    assert(restore(&restore_args) == 0);
    expect_file("data.bin", "v1");
    RestoreBackup nothing = {.user = USER, .dir = &missing};
    assert(restore(&nothing) == ERR_NOT_FOUND);

    /* A second backup replaces the first. */
    assert(mount(&(Mount2){.user = USER, .dir = &dir, .blocks = 64}, &result) == 0);
    write_file("data.bin", "v3");
    assert(umount_backup(&result.point) == 0);
    write_file("data.bin", "v4-after-backup");
    assert(restore(&restore_args) == 0);
    expect_file("data.bin", "v3");
    /* Backup folders and temporaries are hidden: the search still sees one save. */
    Search search = (Search)resolve("sceSaveDataDirNameSearch");
    assert(search);
    DirName names[8];
    SearchCond cond = {.user = USER};
    SearchResult found = {.names = names, .names_capacity = 8};
    assert(search(&cond, &found) == 0 && found.hits == 1 && found.set_count == 1);
    assert(!strcmp(names[0].data, "SAVE0000"));
    assert(nftw(user_root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0); /* synthetic data only */
    puts("PASS: save data backup (umount with backup, check, restore, mount info, hidden)");
    return 0;
}
