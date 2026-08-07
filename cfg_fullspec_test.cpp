// cfg_fullspec_test.cpp — Phase 7: compile the FULL broad_list_composition.spec (all 228759 P /
// 62 T, no locale filter) through the public hs_compile_multi(HS_FLAG_GRAMMAR_REF) path, then a
// real scan of an in-spec engcn sample. Reports wall time (peak RSS via /usr/bin/time -l outside).
#include "hs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
struct Ev { unsigned id; unsigned long long from, to; };
static int cb(unsigned id, unsigned long long f, unsigned long long t, unsigned, void *c){ ((std::vector<Ev>*)c)->push_back({id,f,t}); return 0; }
int main(){
    const char *sp="/Users/fahad/Downloads/Testing/generated/broad_list_name_benchmark/composition/broad_list_composition.spec";
    unsigned flags=HS_FLAG_GRAMMAR_REF, id=0;
    hs_database_t *db=nullptr; hs_compile_error_t *ce=nullptr;
    auto t0=std::chrono::steady_clock::now();
    hs_error_t rv=hs_compile_multi(&sp,&flags,&id,1,HS_MODE_BLOCK,nullptr,&db,&ce);
    auto t1=std::chrono::steady_clock::now();
    double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
    if(rv!=HS_SUCCESS){ printf("FULL COMPILE FAIL rv=%d: %s\n",(int)rv, ce&&ce->message?ce->message:"?"); return 1; }
    printf("FULL spec compiled via public hs_compile_multi(HS_FLAG_GRAMMAR_REF): rv=0 in %.1f ms\n",ms);
    size_t dbsz=0; hs_database_size(db,&dbsz); printf("hs_database_size = %zu bytes\n",dbsz);
    hs_scratch_t *scr=nullptr; hs_alloc_scratch(db,&scr);
    std::vector<Ev> ev; hs_scan(db,"Ai Bai",6,0,scr,cb,&ev);
    int comp=0; for(auto&e:ev) if(e.id>=100000) comp++;
    printf("scan engcn \"Ai Bai\": %d composite match(es):",comp);
    for(auto&e:ev) if(e.id>=100000) printf(" [id=%u @%llu-%llu]",e.id,e.from,e.to);
    printf("\n");
    hs_free_scratch(scr); hs_free_database(db);
    return 0;
}
