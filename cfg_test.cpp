// cfg_test.cpp — links the MODIFIED libhs.a (with src/grammar/cfg_compose compiled in).
// Blocker 2 (re-run vs modified lib) + Blocker 3 (all 6 name_grammars entities) + Blocker 4 (serialize).
#include "grammar/cfg_compose.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace ue2::grammar;
static const std::string DATA="/Users/fahad/Downloads/vectorscan-cfg/cfgdata";

static void scan(Cfg*c,const char*txt){
    std::vector<CfgDet> d; cfgScan(c,txt,std::strlen(txt),d);
    printf("  scan \"%s\" -> %zu: ",txt,d.size());
    for(auto&z:d) printf("[id=%u @%u-%u \"%.*s\"] ",z.id,z.from,z.to,(int)(z.to-z.from),txt+z.from);
    printf("\n");
}
int main(){
    std::string err;
    printf("========== BLOCKER 2+3: compose path, ALL 6 name_grammars entities (modified libhs) ==========\n");
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
    for(auto*l:comp){ if(!cfgAddComposeLine(c,l,err)){printf("  PARSE FAIL: %s\n",err.c_str());return 1;} printf("  parsed: %s\n",l); }
    if(!cfgCompile(c,err)){printf("  COMPILE FAIL: %s\n",err.c_str());return 1;} printf("  compiled rc=0 (modified libhs.a)\n");
    scan(c,"John Smith");           // 9000 namefirstlast (+ 9002 nameinitial: 0 initials)
    scan(c,"Smith, John");          // 9001 namelastcommafirst
    scan(c,"John D. Smith");        // 9002 nameinitial
    scan(c,"John Michael Smith");   // 9003 namefirstmiddlelast
    scan(c,"John Smith Jr");        // 9004 namelastsuffix
    scan(c,"Smith-Jones");          // 9005 compoundlastname
    scan(c,"Smith John");           // wrong-order -> 0 (no namefirstlast)

    printf("\n========== BLOCKER 2: adversarial / depth-1 (modified libhs) ==========\n");
    struct{const char*l;}bad[]={
        {"compose 9000 dup: firstname, lastname"},
        {"compose 9101 nested: namefirstlast, lastname"},
        {"compose 9102 selfref: selfref, lastname"},
        {"compose 9103 miss: firstname, doesnotexist"},
        {"compose 9104 short: firstname"},
        {"compose noid: firstname, lastname"},
    };
    for(auto&b:bad){ std::string e; bool ok=cfgAddComposeLine(c,b.l,e);
        printf("  %-48s -> %s%s\n",b.l,ok?"ACCEPTED":"REJECTED: ",ok?"":e.c_str()); }

    printf("\n========== BLOCKER 2: .spec path (engcn) (modified libhs) ==========\n");
    Cfg*sc=cfgCreate();
    if(!cfgLoadSpec(sc,"/Users/fahad/Downloads/Testing/generated/broad_list_name_benchmark/composition/broad_list_composition.spec","engcn",err)){printf("  SPEC FAIL: %s\n",err.c_str());}
    else{ if(!cfgCompile(sc,err)){printf("  COMPILE FAIL: %s\n",err.c_str());} else{ printf("  spec engcn compiled rc=0\n"); scan(sc,"Ai Bai"); } }
    cfgDestroy(sc);

    printf("\n========== BLOCKER 4: serialization sidecar (offline-compile -> serialize -> deserialize -> serve) ==========\n");
    std::string dbp="/tmp/cfg_test.hsdb", scp="/tmp/cfg_test.sidecar";
    if(!cfgSerialize(c,dbp,scp,err)){printf("  SERIALIZE FAIL: %s\n",err.c_str());return 1;}
    printf("  serialized: db=%s + sidecar=%s\n",dbp.c_str(),scp.c_str());
    Cfg*c2=cfgDeserialize(dbp,scp,err);
    if(!c2){printf("  DESERIALIZE FAIL: %s\n",err.c_str());return 1;}
    printf("  deserialized OK -> re-scan on fresh handle:\n");
    scan(c2,"John Smith");
    scan(c2,"Smith, John");
    scan(c2,"John Michael Smith");
    cfgDestroy(c2); cfgDestroy(c);
    return 0;
}
