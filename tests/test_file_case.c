// SPDX-License-Identifier: GPL-2.0-or-later
/* sotc: guest paths are case-insensitive, as on the PS4 and in shadPS4: the game asks for
 * /app0/pakn.psarc while the dump has pakN.psarc (boot log of 2026-10-16, ADR-0019). On a
 * case-sensitive host the exact path is tried first, then each component is matched without
 * case. The game update overlay (SOTC_UPDATE_DIR) follows the same rule. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void write_file(const char *path,const char *text) {
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void read_back(const char *guest,const char *expected) {
    int fd=(int)do_open(guest,0,0);
    assert(fd>=3);
    char content[16]={0};
    assert(do_read(fd,content,strlen(expected))==(int64_t)strlen(expected));
    assert(!strcmp(content,expected));
    assert(!do_close(fd));
}
int main(void) {
    char root[]="/tmp/sotc-file-case-XXXXXX";
    assert(mkdtemp(root));
    char game[256],update[256],user[256],path[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(update,sizeof(update),"%s/update",root);
    snprintf(user,sizeof(user),"%s/user",root);
    assert(!mkdir(game,0755) && !mkdir(update,0755));
    snprintf(path,sizeof(path),"%s/pakN.psarc",game); write_file(path,"pak");
    snprintf(path,sizeof(path),"%s/Sound",game); assert(!mkdir(path,0755));
    snprintf(path,sizeof(path),"%s/Sound/Shadow",game); assert(!mkdir(path,0755));
    snprintf(path,sizeof(path),"%s/Sound/Shadow/sotc_buss.sxd",game); write_file(path,"bus");
    snprintf(path,sizeof(path),"%s/cachedFiles.psarc",game); write_file(path,"base");
    snprintf(path,sizeof(path),"%s/cachedfiles.psarc",update); write_file(path,"new");
    setenv("SOTC_UPDATE_DIR",update,1);
    runtime_file_configure(game,user);

    read_back("/app0/pakn.psarc","pak");                /* last component */
    read_back("/app0/sound/shadow/SOTC_BUSS.sxd","bus"); /* every component */
    read_back("/app0/pakN.psarc","pak");                /* exact still works */
    read_back("/app0/cachedfiles.psarc","new");          /* the update wins, either case */
    read_back("/app0/CachedFiles.psarc","new");
    GuestStat info;
    assert(!do_stat("/app0/SOUND/shadow",&info));
    assert(do_open("/app0/pakx.psarc",0,0)==-ENOENT);    /* no near miss */
    assert(do_open("/app0/sound/shadow/sotc_buss.sxd/x",0,0)<0);

    /* A new file is created as named; a later open in another case finds it. */
    int fd=(int)do_open("/data/Profile.bin",0x202,0644); assert(fd>=3);
    assert(do_write(fd,"save",4)==4 && !do_close(fd));
    read_back("/data/profile.BIN","save");

    puts("PASS: case-insensitive guest paths (components, update overlay, creation)");
    return 0;
}
