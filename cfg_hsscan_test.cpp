// cfg_hsscan_test.cpp — Phase 4: exercise CFG composition through the REAL public hs_scan callback
// path (NOT cfgScan). Setup uses the internal Cfg builder (Phase 5 replaces this with hs_compile +
// HS_FLAG_GRAMMAR_REF), but the SCAN is the standard hs_scan(db,data,len,flags,scratch,cb,ctx).
#include "grammar/cfg_compose.h"
#include "hs.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace ue2::grammar;
static const std::string DATA="/Users/fahad/Downloads/vectorscan-cfg/cfgdata";

struct Ev { unsigned id; unsigned long long from, to; };
struct Ctx { std::vector<Ev> base, comp; int haltAfter=-1; int seen=0; };

// One user callback for EVERYTHING (base component matches + composites), like a real caller.
static int userCb(unsigned id, unsigned long long from, unsigned long long to, unsigned, void *c){
    Ctx *x=(Ctx*)c;
    if(id>=9000) x->comp.push_back({id,from,to}); else x->base.push_back({id,from,to});
    x->seen++;
    if(x->haltAfter>=0 && x->seen>x->haltAfter) return 1; // request halt
    return 0;
}
static void run(hs_database_t*db, hs_scratch_t*scr, const char*txt, int haltAfter=-1){
    Ctx x; x.haltAfter=haltAfter;
    hs_error_t rv=hs_scan(db,txt,(unsigned)strlen(txt),0,scr,userCb,&x); // <-- REAL public hs_scan
    printf("  hs_scan(\"%s\")%s rv=%d | base=%zu comp=%zu :",
           txt, haltAfter>=0?" [halt]":"", (int)rv, x.base.size(), x.comp.size());
    for(auto&e:x.comp) printf(" [id=%llu @%llu-%llu \"%.*s\"]",(unsigned long long)e.id,e.from,e.to,(int)(e.to-e.from),txt+e.from);
    printf("\n");
}

int main(){
    std::string err;
    Cfg*c=cfgCreate();
    if(!cfgAddEntityFile(c,"firstname",DATA+"/firstname.txt",err)){printf("FAIL %s\n",err.c_str());return 1;}
    if(!cfgAddEntityFile(c,"lastname",DATA+"/lastname.txt",err)){printf("FAIL %s\n",err.c_str());return 1;}
    if(!cfgAddEntityFile(c,"suffix",DATA+"/suffix.txt",err)){printf("FAIL %s\n",err.c_str());return 1;}
    const char* comp[]={
        "compose 9000 namefirstlast: firstname, lastname",
        "compose 9001 namelastcommafirst: lastname, \",\", firstname",
        "compose 9002 nameinitial: firstname, initials, lastname",
        "compose 9003 namefirstmiddlelast: firstname, firstname, lastname",
        "compose 9004 namelastsuffix: firstname, lastname, suffix",
        "compose 9005 compoundlastname: lastname, \"-\", lastname",
    };
    for(auto*l:comp) if(!cfgAddComposeLine(c,l,err)){printf("PARSE FAIL: %s\n",err.c_str());return 1;}
    if(!cfgCompile(c,err)){printf("COMPILE FAIL: %s\n",err.c_str());return 1;}

    // Phase 4: attach metadata + set gate bit, then scan via PUBLIC hs_scan.
    hs_database_t*db=cfgFinalizeForScan(c);
    hs_scratch_t*scr=cfgScratch(c);
    printf("========== PHASE 4: composites via REAL hs_scan (id>=9000) ==========\n");
    run(db,scr,"John Smith");          // 9000 (and 9002 nameinitial w/ 0 initials — documented)
    run(db,scr,"Smith, John");         // 9001
    run(db,scr,"John D. Smith");       // 9002
    run(db,scr,"John Michael Smith");  // 9003
    run(db,scr,"John Smith Jr");       // 9004
    run(db,scr,"Smith-Jones");         // 9005
    run(db,scr,"Smith John");          // wrong order -> 0 composites

    printf("\n========== PHASE 4: base + composite callback events (one scan) ==========\n");
    { Ctx x; hs_scan(db,"John Smith",10,0,scr,userCb,&x);
      printf("  base events (%zu):",x.base.size()); for(auto&e:x.base) printf(" id=%llu@%llu-%llu",(unsigned long long)e.id,e.from,e.to);
      printf("\n  comp events (%zu):",x.comp.size()); for(auto&e:x.comp) printf(" id=%llu@%llu-%llu",(unsigned long long)e.id,e.from,e.to); printf("\n"); }

    printf("\n========== PHASE 4: early-return (user returns non-zero on first base match) ==========\n");
    run(db,scr,"John Michael Smith",0); // halt after first callback -> expect HS_SCAN_TERMINATED(2), no composites

    cfgDestroy(c); // triggers hs_free_database -> cfg_unregister_hook (side-table cleanup)
    return 0;
}
