// cfg_compile_test.cpp — Phase 5: the CALLER uses ONLY public hs_* APIs. No cfg* symbol appears.
// hs_compile_multi(expr=".hsg", HS_FLAG_GRAMMAR_REF) -> hs_database_t*, then standard hs_scan.
#include "hs.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Ev { unsigned id; unsigned long long from, to; };
static int cb(unsigned id, unsigned long long from, unsigned long long to, unsigned, void *c){
    ((std::vector<Ev>*)c)->push_back({id,from,to}); return 0;
}
static void scan(hs_database_t*db, hs_scratch_t*scr, const char*txt){
    std::vector<Ev> ev; hs_scan(db,txt,(unsigned)strlen(txt),0,scr,cb,&ev);
    printf("  hs_scan(\"%s\"): composites:",txt);
    for(auto&e:ev) if(e.id>=9000) printf(" [id=%u @%llu-%llu \"%.*s\"]",e.id,e.from,e.to,(int)(e.to-e.from),txt+e.from);
    printf("\n");
}
int main(){
    // ---------- Front-door A: compose .hsg via HS_FLAG_GRAMMAR_REF ----------
    const char *expr = "/Users/fahad/Downloads/vectorscan-cfg/cfgdata/names.hsg";
    unsigned flags = HS_FLAG_GRAMMAR_REF, id = 0;
    hs_database_t *db=nullptr; hs_compile_error_t *ce=nullptr;
    hs_error_t rv = hs_compile_multi(&expr,&flags,&id,1,HS_MODE_BLOCK,nullptr,&db,&ce);
    if(rv!=HS_SUCCESS){ printf("COMPILE FAIL rv=%d: %s\n",rv, ce&&ce->message?ce->message:"?"); return 1; }
    printf("hs_compile_multi(.hsg, HS_FLAG_GRAMMAR_REF) rv=0, db=%p\n",(void*)db);
    hs_scratch_t *scr=nullptr;
    if(hs_alloc_scratch(db,&scr)!=HS_SUCCESS){ printf("scratch fail\n"); return 1; }
    printf("========== PHASE 5: composites via public hs_compile + hs_scan (.hsg front-door) ==========\n");
    scan(db,scr,"John Smith");
    scan(db,scr,"Smith, John");
    scan(db,scr,"John D. Smith");
    scan(db,scr,"John Michael Smith");
    scan(db,scr,"John Smith Jr");
    scan(db,scr,"Smith-Jones");
    scan(db,scr,"Smith John");
    hs_free_scratch(scr); hs_free_database(db);

    // ---------- Front-door B: .spec:locale via HS_FLAG_GRAMMAR_REF ----------
    const char *sp = "/Users/fahad/Downloads/Testing/generated/broad_list_name_benchmark/composition/broad_list_composition.spec:engcn";
    hs_database_t *db2=nullptr; hs_compile_error_t *ce2=nullptr;
    hs_error_t rv2 = hs_compile_multi(&sp,&flags,&id,1,HS_MODE_BLOCK,nullptr,&db2,&ce2);
    if(rv2!=HS_SUCCESS){ printf(".spec COMPILE FAIL rv=%d: %s\n",rv2, ce2&&ce2->message?ce2->message:"?"); return 1; }
    hs_scratch_t *scr2=nullptr; hs_alloc_scratch(db2,&scr2);
    printf("\n========== PHASE 5: composites via public hs_compile + hs_scan (.spec:engcn front-door) ==========\n");
    { std::vector<Ev> ev; hs_scan(db2,"Ai Bai",6,0,scr2,cb,&ev);
      printf("  hs_scan(\"Ai Bai\"): composites:"); for(auto&e:ev) if(e.id>=100000) printf(" [id=%u @%llu-%llu]",e.id,e.from,e.to); printf("\n"); }
    hs_free_scratch(scr2); hs_free_database(db2);
    return 0;
}
