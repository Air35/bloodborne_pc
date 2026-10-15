/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
/* sotc: `blocks` remembers the size asked at mount, for sceSaveDataGetMountInfo (docs/plans/
 * 2026-10-15-a3-hle-lote5.md). */
static struct { int used; char host[700], meta[700]; uint64_t blocks; } slots[SLOTS];
static char memory_path[720];
static uint64_t memory_size;
static size_t mounts_done, memory_writes;

/* Bloodborne "sound hack" (rainvmaker, from the Diegolix29 shadPS4 fork): the game data
 * save carries a flag at 0x204E of userdata0010; with it clear, parts of the game's audio
 * (e.g. the player's weapon sounds) never play. It is set before the game reads the save.
 * BB_SOUND_HACK=0 leaves saves untouched. */
static void bloodborne_sound_hack(void) {
    static const char *const ids[]={"CUSA00207","CUSA00208","CUSA00299","CUSA00900","CUSA01363","CUSA03014","CUSA03023","CUSA03173","CUSA03179"};
    const char *env=getenv("BB_SOUND_HACK");
    if (env && env[0]=='0') return;
    int bloodborne=0;
    for (size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) if (!strcmp(title_id,ids[i])) bloodborne=1;
    if (!bloodborne) return;
    char users[700];
    snprintf(users,sizeof(users),"%s/savedata",runtime_file_user_dir());
    DIR *dir=opendir(users);
    if (!dir) return;
    for (struct dirent *e; (e=readdir(dir));) {
        if (e->d_name[0]=='.') continue;
        char path[1100];
        snprintf(path,sizeof(path),"%s/%s/%s/SPRJ0005/userdata0010",users,e->d_name,title_id);
        FILE *f=fopen(path,"r+b");
        if (!f) continue;
        int old=fseek(f,0x204E,SEEK_SET) ? EOF : fgetc(f);
        if (old!=EOF && old!=1 && !fseek(f,0x204E,SEEK_SET)) {
            fputc(1,f);
            printf("Runtime: Bloodborne sound flag set in %s (was %d)\n",path,old);
        }
        fclose(f);
    }
    closedir(dir);
}
void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
    bloodborne_sound_hack();
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
/* A whole file replaced in one rename (a crash mid-write keeps the old one). */
static int write_atomic(const char *path, const void *data, size_t size) {
    char temp[760]; snprintf(temp,sizeof(temp),"%s.bbtmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size && !fflush(f) && !fsync(fileno(f));
    ok=!fclose(f) && ok;
    if (!ok || rename(temp,path)) { unlink(temp); return -1; }
    return 0;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    Param copy=*p; copy.mtime=time(NULL);
    write_atomic(path,&copy,sizeof(copy));
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
/* sotc: the request is one struct (the old signature would take a sixth parameter for `blocks`). */
typedef struct { int32_t user; const char *title; const DirName *dir; uint32_t mode; uint64_t blocks; } MountRequest;
static int32_t mount(const MountRequest *rq, MountResult *result) {
    const DirName *dir=rq->dir;
    const uint32_t mode=rq->mode;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (rq->user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(rq->user,rq->title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    slots[slot].blocks=rq->blocks;
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    const MountRequest rq={m->user,m->title ? m->title->data : NULL,m->dir,m->mode,m->blocks};
    return mount(&rq,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    const MountRequest rq={m->user,NULL,m->dir,m->mode,m->blocks};
    return mount(&rq,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        r=write_atomic(path,icon->buffer,icon->data_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    nftw(host,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    nftw(meta,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* --- sotc: backup of a save (docs/plans/2026-10-15-a3-hle-lote5.md) ---------------------------
 * <base>/.<dir>.backup/data is a copy of the save directory and .../meta a copy of
 * <dir>.sce_sys. The dot keeps backup and temporaries out of save_search (it skips dot names).
 * Contract: shadPS4 b4e7ae7 save_data/savedata.cpp:587-640 (CheckBackupData), 1322-1360
 * (RestoreBackupData), 1715-1718 (UmountWithBackup), 995-1017 (GetMountInfo), consulted. The
 * on-disk layout is ours: the reference keeps it in a module that was not consulted. Each tree
 * is built beside the old one and swapped in by rename, so a failure keeps the previous copy.
 * Backups are synchronous: the game's wait-and-retry on 0x809F0013 (eboot+0xa49da7, 0xa48f82;
 * BACKUP_BUSY by its behaviour [a verificar]) never has a reason to run. Deleting a save does
 * not delete its backup [a verificar: console behaviour]. */
enum { TREE_DEPTH=8, SAVE_BLOCK_SIZE=32768 };
typedef struct { void *buffer; uint64_t capacity, size; uint8_t reserved[32]; } IconOut;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param;
                 IconOut *icon; uint8_t reserved[32]; } CheckBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint32_t unused; uint8_t reserved[32]; int32_t pad2; } RestoreBackup;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } MountInfo;
typedef struct { char root[720], data[740], meta[740]; } BackupPaths;
_Static_assert(sizeof(CheckBackup)==72 && sizeof(RestoreBackup)==72 && sizeof(MountInfo)==48,"backup layouts");

static int is_dir(const char *path) {
    struct stat st;
    return !stat(path,&st) && S_ISDIR(st.st_mode);
}
static int copy_file(const char *from, const char *to) {
    FILE *in=fopen(from,"rb"), *out=in ? fopen(to,"wb") : NULL;
    int ok=in && out;
    char buffer[16384];
    for (size_t n; ok && (n=fread(buffer,1,sizeof(buffer),in))>0;) ok=fwrite(buffer,1,n,out)==n;
    ok=ok && !ferror(in);
    if (out) ok=!fflush(out) && !fsync(fileno(out)) && ok;
    if (in) fclose(in);
    if (out) ok=!fclose(out) && ok;
    return ok ? 0 : -1;
}
/* Directories and regular files only (a symlink in a save is not followed or copied). */
static int copy_tree(const char *from, const char *to, int depth) {
    DIR *d=opendir(from);
    if (!d) return -1;
    int rc=mkdir(to,0755) && errno!=EEXIST ? -1 : 0;
    for (struct dirent *e; !rc && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char src[760], dst[760];
        struct stat st;
        if ((size_t)snprintf(src,sizeof(src),"%s/%s",from,e->d_name)>=sizeof(src) ||
            (size_t)snprintf(dst,sizeof(dst),"%s/%s",to,e->d_name)>=sizeof(dst) || lstat(src,&st)) rc=-1;
        else if (S_ISDIR(st.st_mode)) rc=depth<TREE_DEPTH ? copy_tree(src,dst,depth+1) : -1;
        else if (S_ISREG(st.st_mode)) rc=copy_file(src,dst);
    }
    closedir(d);
    return rc;
}
static uint64_t tree_bytes(const char *path, int depth) {
    DIR *d=opendir(path);
    if (!d) return 0;
    uint64_t total=0;
    for (struct dirent *e; (e=readdir(d));) {
        char child[760];
        struct stat st;
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if ((size_t)snprintf(child,sizeof(child),"%s/%s",path,e->d_name)>=sizeof(child) || lstat(child,&st)) continue;
        if (S_ISREG(st.st_mode)) total+=(uint64_t)st.st_size;
        else if (S_ISDIR(st.st_mode) && depth<TREE_DEPTH) total+=tree_bytes(child,depth+1);
    }
    closedir(d);
    return total;
}
static void remove_tree(const char *path) {
    nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS);
}
/* "<dir>/<name>" -> "<dir>/.<name><suffix>": a hidden sibling on the same filesystem. */
static int hidden_sibling(const char *path, const char *suffix, char *out, size_t size) {
    const char *slash=strrchr(path,'/');
    if (!slash) return -1;
    const int n=snprintf(out,size,"%.*s/.%s%s",(int)(slash-path),path,slash+1,suffix);
    return n<0 || (size_t)n>=size ? -1 : 0;
}
/* Makes `to` a copy of `from`: built as a hidden sibling, then swapped in (the old one is moved
 * aside first and put back if the final rename fails). */
static int replace_tree(const char *from, const char *to) {
    char fresh[780], old[780];
    if (hidden_sibling(to,".new",fresh,sizeof(fresh)) || hidden_sibling(to,".old",old,sizeof(old))) return -1;
    remove_tree(fresh);
    remove_tree(old);
    if (copy_tree(from,fresh,0)) { remove_tree(fresh); return -1; }
    const int had=is_dir(to);
    if (had && rename(to,old)) { remove_tree(fresh); return -1; }
    if (rename(fresh,to)) {
        if (had) rename(old,to);
        remove_tree(fresh);
        return -1;
    }
    remove_tree(old);
    return 0;
}
static int backup_paths(const char *host, BackupPaths *b) {
    if (hidden_sibling(host,".backup",b->root,sizeof(b->root))) return -1;
    return (size_t)snprintf(b->data,sizeof(b->data),"%s/data",b->root)>=sizeof(b->data) ||
           (size_t)snprintf(b->meta,sizeof(b->meta),"%s/meta",b->root)>=sizeof(b->meta) ? -1 : 0;
}
static int make_backup(const char *host, const char *meta) {
    BackupPaths b;
    if (backup_paths(host,&b) || (mkdir(b.root,0755) && errno!=EEXIST)) return -1;
    if (replace_tree(host,b.data)) return -1;
    return is_dir(meta) && replace_tree(meta,b.meta) ? -1 : 0;
}
static ABI int32_t save_umount_with_backup(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[700], meta[700];
    pthread_mutex_lock(&lock); /* held over the copy: a new mount of this save waits for it */
    const int slot=slot_of(point);
    if (slot>=0) {
        snprintf(host,sizeof(host),"%s",slots[slot].host);
        snprintf(meta,sizeof(meta),"%s",slots[slot].meta);
        runtime_file_unmount(point->data);
        slots[slot].used=0;
    }
    const int rc=slot>=0 ? make_backup(host,meta) : 0;
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_FOUND;
    return rc ? ERR_INTERNAL : 0;
}
/* Resolves the save of `user`/`title`/`dir` and its backup paths; the save must not be mounted
 * (the reference answers BUSY; its value was not in the consulted text, so ERR_BAD_MOUNTED).
 * Called with `lock` held. */
static int32_t backup_lookup(int32_t user, const char *title, const DirName *dir, char *host,
                             size_t size, BackupPaths *b) {
    if (!dir || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    if (user<0) return ERR_INVALID_USER;
    char base[600];
    root(user,title,base,sizeof(base));
    if ((size_t)snprintf(host,size,"%s/%s",base,dir->data)>=size || backup_paths(host,b)) return ERR_PARAMETER;
    for (int i=0;i<SLOTS;++i) if (slots[i].used && !strcmp(slots[i].host,host)) return ERR_BAD_MOUNTED;
    return is_dir(b->data) ? 0 : ERR_NOT_FOUND;
}
/* The icon the caller asked for: as much of the backup's icon0.png as fits its buffer. */
static void load_backup_icon(const char *meta, IconOut *icon) {
    char path[780];
    if (!icon || !icon->buffer) return;
    icon->size=0;
    if ((size_t)snprintf(path,sizeof(path),"%s/icon0.png",meta)>=sizeof(path)) return;
    FILE *f=fopen(path,"rb");
    if (!f) return;
    icon->size=fread(icon->buffer,1,icon->capacity,f);
    fclose(f);
}
static ABI int32_t save_check_backup(const CheckBackup *check) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!check || !check->dir) return ERR_PARAMETER;
    char host[700];
    BackupPaths b;
    pthread_mutex_lock(&lock);
    int32_t r=backup_lookup(check->user,check->title ? check->title->data : NULL,check->dir,host,sizeof(host),&b);
    if (!r && check->param && read_param(b.meta,check->param)) r=ERR_INTERNAL;
    if (!r) load_backup_icon(b.meta,check->icon);
    pthread_mutex_unlock(&lock);
    return r;
}
static int restore_trees(const char *host, const BackupPaths *b) {
    char meta[780];
    if (replace_tree(b->data,host)) return -1;
    if (!is_dir(b->meta)) return 0;
    if ((size_t)snprintf(meta,sizeof(meta),"%s.sce_sys",host)>=sizeof(meta)) return -1;
    return replace_tree(b->meta,meta);
}
static ABI int32_t save_restore_backup(const RestoreBackup *restore) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!restore || !restore->dir) return ERR_PARAMETER;
    char host[700];
    BackupPaths b;
    pthread_mutex_lock(&lock);
    int32_t r=backup_lookup(restore->user,restore->title ? restore->title->data : NULL,restore->dir,host,sizeof(host),&b);
    if (!r && restore_trees(host,&b)) r=ERR_INTERNAL;
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data '%s' restored from its backup\n",restore->dir->data);
    return r;
}
/* Total blocks as asked at mount; free = total - used / block size, as the reference. The SotC
 * reads `blocks` << 15 (eboot+0xa4aa98..0xa4aa9c); what it does with that is [a verificar]. */
static ABI int32_t save_mount_info(const MountPoint *point, MountInfo *info) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !info) return ERR_PARAMETER;
    char host[700];
    uint64_t blocks=0;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(point);
    if (slot>=0) {
        snprintf(host,sizeof(host),"%s",slots[slot].host);
        blocks=slots[slot].blocks;
    }
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    const uint64_t used=tree_bytes(host,0)/SAVE_BLOCK_SIZE;
    memset(info,0,sizeof(*info));
    info->blocks=blocks;
    info->free_blocks=blocks>used ? blocks-used : 0;
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys"))) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    if (d) closedir(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title. */
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size) return ERR_PARAMETER;
    char base[600], dir[640];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memory_path,sizeof(memory_path),"%s/memory.dat",dir);
    FILE *f=fopen(memory_path,"r+b");
    if (!f) f=fopen(memory_path,"w+b");
    int32_t r=ERR_INTERNAL;
    if (f) {
        fseek(f,0,SEEK_END);
        if ((uint64_t)ftell(f)<size && ftruncate(fileno(f),(off_t)size)) goto done;
        memory_size=size; r=0;
    done:
        fclose(f);
    }
    if (!r && param) write_param(dir,param);
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes)\n",(unsigned long long)size);
    return r;
}
static int32_t memory_io(void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory_size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,1); }

static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search},
    {"sceSaveDataUmountWithBackup",save_umount_with_backup}, {"sceSaveDataCheckBackupData",save_check_backup},
    {"sceSaveDataRestoreBackupData",save_restore_backup}, {"sceSaveDataGetMountInfo",save_mount_info},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }
#else
void runtime_savedata_configure(const char *title) { (void)title; }
uintptr_t runtime_savedata_resolve(const char *name) { (void)name; return 0; }
void runtime_savedata_report(void) {}
#endif
