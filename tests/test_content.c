#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
/* The user folder add-ons are listed from (runtime_file.c in the game): a temporary one here. */
static char user_dir[64]="/tmp/bbport-content-test-XXXXXX";
const char *runtime_file_user_dir(void) { return user_dir; }
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
#define GET(t,n) ((t)runtime_content_resolve(n))
int main(int argc,char **argv) {
    assert(mkdtemp(user_dir));
    Module load=GET(Module,"g8cM39EUZ6o#libSceSysmodule");
    Module loaded=GET(Module,"fMP5NHUOaMk#libSceSysmodule");
    Module unload=GET(Module,"eR2bZFAAU0Q#libSceSysmodule");
    Init init=GET(Init,"R9lA82OraNs#libSceAppContent");
    Param param=GET(Param,"99b82IKXpH4#libSceAppContent");
    List list=GET(List,"xnd8BJzAxmk#libSceAppContent");
    assert(load && loaded && unload && init && param && list);
    /* sotc: the NID alone decides (runtime.c of the union): the per-executable #A#B letters of
     * the prepared image and the #library spelling resolve alike; a bare NID too. */
    assert(GET(Module,"g8cM39EUZ6o#C#D")==load && GET(Module,"g8cM39EUZ6o")==load);
    assert(!runtime_content_resolve("g8cM39EUZ6") && !runtime_content_resolve("g8cM39EUZ6oX"));
    if (argc>1 && !strcmp(argv[1],"--missing")) { load(0xb4); return 99; }
    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        /* sotc: sceSysmoduleLoadModuleInternalWithArg (hHrGoGoNf+s), called by libSceFios2 with
         * id 0xd7: same host-provided, reference-counted modules; ids with bit 31 are the
         * internal ones and do not alias the public id of the same low bits. */
        typedef int32_t (ABI *Internal)(uint32_t,int32_t,const void *,uint64_t,int32_t *);
        Internal internal=GET(Internal,"hHrGoGoNf+s#libSceSysmodule");
        assert(internal);
        int32_t result=-1;
        assert(internal(0xd7,0,NULL,0,&result)==0 && result==0 && loaded(0xd7)==0);
        assert(unload(0xd7)==0 && (uint32_t)loaded(0xd7)==0x805a1001);
        assert(internal(0xd7,0,NULL,0,NULL)==0 && unload(0xd7)==0);          /* res_out is optional */
        assert((uint32_t)internal(0,0,NULL,0,&result)==0x805a1000);          /* INVALID_ID */
        assert((uint32_t)internal(0x80000000u,0,NULL,0,&result)==0x805a1000);
        assert((uint32_t)internal(0xd7,0,NULL,1,&result)==0x805a1000);       /* unk must be 0 */
        assert(internal(0x80000039u,0,NULL,0,&result)==0);
        assert((uint32_t)loaded(0x39)==0x805a1001);                          /* not the public 0x39 */
        puts("PASS: independent module references, internal load"); return 0;
    }
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    /* <user>/addcont/<title>/<label>: reported installed (The Old Hunters' entitlement). */
    char path[160];
    snprintf(path,sizeof(path),"%s/addcont",user_dir); assert(!mkdir(path,0755));
    snprintf(path,sizeof(path),"%s/addcont/TEST00000",user_dir); assert(!mkdir(path,0755)); /* sotc: fictitious title (ADR-0016) */
    snprintf(path,sizeof(path),"%s/addcont/TEST00000/SPEXPANSIONDLC03",user_dir); assert(!mkdir(path,0755));
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==1);
    memset(entries,0xaa,sizeof(entries));
    assert(list(0,entries,2,&h.hits)==0 && h.hits==1);
    assert(!strcmp((const char *)entries,"SPEXPANSIONDLC03"));
    uint32_t status; memcpy(&status,entries+20,4); assert(status==4);
    for (unsigned i=24;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    rmdir(path);
    snprintf(path,sizeof(path),"%s/addcont/TEST00000",user_dir); rmdir(path);
    snprintf(path,sizeof(path),"%s/addcont",user_dir); rmdir(path);
    rmdir(user_dir);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile, add-on folders and output boundaries");
    return 0;
}
