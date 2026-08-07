// cfg_sidecar_test.cpp — Phase 6: compile -> public hs_serialize_database + sidecar -> free ->
// fresh hs_deserialize_database -> (missing-metadata error) -> cfgAttachSidecar -> public hs_scan.
// The scan is standard hs_scan; only the attach is the documented internal mechanism (deserialize
// has no path, so the sidecar cannot be auto-discovered).
#include "hs.h"
#include "grammar/cfg_compose.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace ue2::grammar;

struct Ev { unsigned id; unsigned long long from, to; };
static int cb(unsigned id, unsigned long long f, unsigned long long t, unsigned, void *c){ ((std::vector<Ev>*)c)->push_back({id,f,t}); return 0; }
static void showComposites(hs_database_t*db, hs_scratch_t*scr, const char*txt){
    std::vector<Ev> ev; hs_error_t rv=hs_scan(db,txt,(unsigned)strlen(txt),0,scr,cb,&ev);
    printf("  hs_scan(\"%s\") rv=%d composites:",txt,(int)rv);
    for(auto&e:ev) if(e.id>=9000) printf(" [id=%u @%llu-%llu \"%.*s\"]",e.id,e.from,e.to,(int)(e.to-e.from),txt+e.from);
    printf("\n");
}
int main(){
    const char *expr="/Users/fahad/Downloads/vectorscan-cfg/cfgdata/names.hsg";
    unsigned flags=HS_FLAG_GRAMMAR_REF, id=0;
    hs_database_t *db=nullptr; hs_compile_error_t *ce=nullptr;
    if(hs_compile_multi(&expr,&flags,&id,1,HS_MODE_BLOCK,nullptr,&db,&ce)!=HS_SUCCESS){ printf("compile fail\n"); return 1; }

    std::string dbp="/tmp/cfg6.hsdb", scp="/tmp/cfg6.sidecar", err;
    char *bytes=nullptr; size_t blen=0;
    if(hs_serialize_database(db,&bytes,&blen)!=HS_SUCCESS){ printf("serialize fail\n"); return 1; }
    { std::ofstream o(dbp,std::ios::binary); o.write(bytes,(std::streamsize)blen); } free(bytes);
    if(!cfgSerializeSidecar(db,scp,err)){ printf("sidecar write fail: %s\n",err.c_str()); return 1; }
    printf("serialized: db=%s (%zu bytes) + sidecar=%s\n",dbp.c_str(),blen,scp.c_str());
    hs_free_database(db); // drops the side-table entry (unregister)

    // fresh load (no path -> cannot auto-discover sidecar)
    std::ifstream df(dbp,std::ios::binary); std::string buf((std::istreambuf_iterator<char>(df)),std::istreambuf_iterator<char>());
    hs_database_t *db2=nullptr;
    if(hs_deserialize_database(buf.data(),buf.size(),&db2)!=HS_SUCCESS){ printf("deserialize fail\n"); return 1; }
    hs_scratch_t *scr=nullptr; hs_alloc_scratch(db2,&scr);

    printf("\n========== PHASE 6a: CFG DB deserialized but sidecar NOT attached (must NOT silent-drop) ==========\n");
    { std::vector<Ev> ev; hs_error_t rv=hs_scan(db2,"John Smith",10,0,scr,cb,&ev);
      printf("  hs_scan before attach -> rv=%d (expect HS_INVALID=%d), composites=%zu\n",(int)rv,(int)HS_INVALID,ev.size()); }

    printf("\n========== PHASE 6b: attach sidecar (documented internal mechanism) then public hs_scan ==========\n");
    if(cfgAttachSidecar(db2,scp,err)!=HS_SUCCESS){ printf("attach fail: %s\n",err.c_str()); return 1; }
    printf("  cfgAttachSidecar ok\n");
    showComposites(db2,scr,"John Smith");
    showComposites(db2,scr,"Smith, John");
    showComposites(db2,scr,"John Michael Smith");

    printf("\n========== PHASE 6c: attach a MISSING sidecar file -> clean error ==========\n");
    { std::string e2; int rv=cfgAttachSidecar(db2,"/tmp/does_not_exist.sidecar",e2);
      printf("  cfgAttachSidecar(missing) -> rv=%d (%s)\n",rv,e2.c_str()); }

    hs_free_scratch(scr); hs_free_database(db2);
    return 0;
}
