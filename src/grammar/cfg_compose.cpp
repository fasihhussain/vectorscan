/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cfg_compose.cpp - see cfg_compose.h. Position-join ported from vs_chain_bench.cpp
 * (connector-gap validators + join loop). Plain string handling only.
 */

#include "grammar/cfg_compose.h"
#include "grammar/cfg_runtime.h"

#include "hs.h"
#include "database.h" // struct hs_database (reserved0 gate bit)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ue2 {
namespace grammar {

static const int MAXGAP = 12;
enum Conn { NONE, SPACE, SPACE_OPT, SPACE_MUST, INITIALS, COMMA_INIT, NAME, LITERAL };

// ---- connector-gap validators (ported ~verbatim from vs_chain_bench.cpp :44-117) ----
static int spaceItemLen(const std::string &s, size_t i, size_t end) {
    unsigned char c = (unsigned char)s[i];
    if (c=='\t'||c==' '||c=='.'||c=='-'||c=='/'||c==','||c==';'||c=='|') return 1;
    if (i+2<end&&(unsigned char)s[i]==0xE2&&(unsigned char)s[i+1]==0x80&&(unsigned char)s[i+2]==0x93) return 3;
    if (i+2<end&&s[i]==':'&&s[i+1]==':'&&s[i+2]=='\'') return 3;
    return 0;
}
static bool consumeItems(const std::string &s, size_t pos, size_t end, int used, int lo, int hi, bool nl) {
    if (pos==end) return used>=lo&&used<=hi;
    if (used>=hi) return false;
    int n=spaceItemLen(s,pos,end);
    if (n>0&&consumeItems(s,pos+n,end,used+1,lo,hi,nl)) return true;
    if (nl&&s[pos]=='\n'&&consumeItems(s,pos+1,end,used+1,lo,hi,nl)) return true;
    return false;
}
static bool commaSpaces(const std::string &s, size_t from, size_t end, int lo, int hi) {
    size_t i=from; if (i<end&&s[i]==',') i++;
    int sp=0; while (i<end&&s[i]==' ') { i++; sp++; }
    return i==end&&sp>=lo&&sp<=hi;
}
static bool initialsGap(const std::string &s, size_t from, size_t to) {
    size_t i=from; int k=0;
    while (k<3&&i+1<to&&s[i]==' '&&s[i+1]>='A'&&s[i+1]<='Z') { i+=2; if (i<to&&s[i]=='.') i++; k++; }
    if (i>=to||s[i]!=' ') return false; i++; return i==to;
}
static bool nameGap(const std::string &s, size_t from, size_t to) {
    size_t i=from; if (i>=to||s[i]!=' ') return false; i++; return i==to;
}
static bool literalGap(const std::string &s, size_t from, size_t to, const std::string &lit) {
    size_t i=from; while (i<to&&s[i]==' ') i++;
    if (i+lit.size()>to||s.compare(i,lit.size(),lit)!=0) return false; i+=lit.size();
    while (i<to&&s[i]==' ') i++; return i==to;
}
static bool connOk(int c, const std::string &s, size_t from, size_t to, const std::string &lit) {
    switch (c) {
        case NONE:       return from==to;
        case NAME:       return nameGap(s,from,to);
        case SPACE:      return consumeItems(s,from,to,0,1,3,true)||commaSpaces(s,from,to,1,10);
        case SPACE_MUST: return consumeItems(s,from,to,0,1,3,false)||commaSpaces(s,from,to,1,10);
        case SPACE_OPT:  return from==to||consumeItems(s,from,to,0,0,3,false)||commaSpaces(s,from,to,0,10);
        case INITIALS:   return initialsGap(s,from,to);
        case COMMA_INIT: return from<to&&s[from]==','&&initialsGap(s,from+1,to);
        case LITERAL:    return literalGap(s,from,to,lit);
        default: return false;
    }
}
static inline bool isWord(unsigned char c){return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';}
static std::string trim(const std::string &s){size_t a=s.find_first_not_of(" \t\r\n");if(a==std::string::npos)return"";size_t b=s.find_last_not_of(" \t\r\n");return s.substr(a,b-a+1);}

struct Step { int conn; std::string entity; std::string lit; };
struct Template { unsigned id; std::string name; std::vector<Step> steps; };

class Cfg {
public:
    std::map<std::string,std::vector<std::string>> ents;   // flat entity -> literals
    std::set<std::string> composeNames;
    std::set<unsigned> ids; std::set<std::string> names;
    std::vector<Template> templates;
    // post-compile
    hs_database_t *db=nullptr; hs_scratch_t *scr=nullptr;
    std::vector<int> id2type; std::vector<uint32_t> litLen;
    std::vector<std::string> typeName; std::map<std::string,int> typeIdx;
    ~Cfg(){ if(scr)hs_free_scratch(scr); if(db)hs_free_database(db); }
};

// ---- scan-time composition metadata, attached to a compiled hs_database_t via a side-table ----
// (hs_database_t has a fixed serialized layout + CRC and cannot grow a live pointer field, so an
//  external table keyed by the DB pointer is used instead. Immutable after register; only the
//  map itself is mutex-guarded for register/lookup/unregister. No mutation during a scan.)
struct CfgMeta {
    std::vector<int> id2type;          // component pattern id -> type index
    std::vector<uint32_t> litLen;      // component pattern id -> literal length (no-SOM start calc)
    std::vector<std::string> typeName; // type index -> entity name
    std::map<std::string,int> typeIdx; // entity name -> type index
    std::vector<Template> templates;   // compose templates (position-join plan)
};
static std::map<const hs_database_t*, CfgMeta> g_cfgReg;
static std::mutex g_cfgMtx;

// External-linkage: armed into cfg_unregister_hook when the first CFG DB registers (Phase 4/5).
extern "C" void cfg_unregister_impl(const void *db) {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    g_cfgReg.erase(static_cast<const hs_database_t*>(db));
}

Cfg *cfgCreate(){ return new Cfg(); }
int cfgTemplateCount(const Cfg *c){ return (int)c->templates.size(); }
void cfgDestroy(Cfg *c){ delete c; }

bool cfgAddEntityFile(Cfg *c, const std::string &entity, const std::string &path, std::string &err){
    std::ifstream f(path); if(!f){ err="cannot open dict "+path; return false; }
    std::vector<std::string> v; std::string l;
    while(std::getline(f,l)){ if(!l.empty()&&l.back()=='\r') l.pop_back(); if(!trim(l).empty()) v.push_back(l); }
    if(v.empty()){ err="empty dict "+path; return false; }
    c->ents[entity]=std::move(v); return true;
}
static int connKeyword(const std::string &s,bool &isConn){
    isConn=true;
    if(s=="space")return SPACE; if(s=="space_must")return SPACE_MUST; if(s=="space_opt")return SPACE_OPT;
    if(s=="initials")return INITIALS; if(s=="comma_init")return COMMA_INIT; if(s=="name")return NAME;
    isConn=false; return SPACE;
}
bool cfgAddComposeLine(Cfg *c, const std::string &line, std::string &err){
    std::istringstream is(line); std::string kw; is>>kw; if(kw!="compose"){ err="not a compose line"; return false; }
    unsigned id; if(!(is>>id)){ err="compose: missing numeric id"; return false; }
    std::string rest; std::getline(is,rest); size_t col=rest.find(':'); if(col==std::string::npos){ err="compose: missing ':'"; return false; }
    std::string name=trim(rest.substr(0,col)), items_s=rest.substr(col+1);
    if(name.empty()){ err="compose: empty name"; return false; }
    if(c->ids.count(id)){ err="compose: duplicate id "+std::to_string(id); return false; }
    if(c->names.count(name)){ err="compose: duplicate name "+name; return false; }
    std::vector<std::string> items; std::string cur; bool inq=false;
    for(char ch:items_s){ if(ch=='"'){inq=!inq;cur+=ch;} else if(ch==','&&!inq){items.push_back(trim(cur));cur.clear();} else cur+=ch; }
    if(!trim(cur).empty()) items.push_back(trim(cur));
    Template t; t.id=id; t.name=name; int pending=SPACE; std::string pendingLit; bool havePending=false;
    for(auto &it:items){ if(it.empty()) continue;
        if(it.size()>=2&&it.front()=='"'&&it.back()=='"'){ pending=LITERAL; pendingLit=it.substr(1,it.size()-2); havePending=true; continue; }
        bool isC; int ck=connKeyword(it,isC); if(isC){ pending=ck; pendingLit.clear(); havePending=true; continue; }
        Step st; st.entity=it; st.lit=pendingLit; st.conn=t.steps.empty()?NONE:(havePending?pending:SPACE);
        t.steps.push_back(st); pending=SPACE; pendingLit.clear(); havePending=false;
    }
    if(t.steps.size()<2){ err="compose "+name+": needs >=2 components"; return false; }
    for(auto &st:t.steps){
        if(st.entity==name){ err="compose "+name+": self-reference"; return false; }
        if(c->composeNames.count(st.entity)){ err="compose "+name+": references composite '"+st.entity+"' (depth>1 not allowed)"; return false; }
        if(!c->ents.count(st.entity)){ err="compose "+name+": references missing/empty entity '"+st.entity+"'"; return false; }
    }
    c->ids.insert(id); c->names.insert(name); c->composeNames.insert(name); c->templates.push_back(std::move(t));
    return true;
}
static int specConn(const std::string &s){ if(s=="-"||s.empty())return NONE; if(s=="initials")return INITIALS;
    if(s=="comma_init")return COMMA_INIT; if(s=="space")return SPACE; if(s=="space_must")return SPACE_MUST;
    if(s=="space_opt")return SPACE_OPT; if(s=="name")return NAME; return SPACE; }
bool cfgLoadSpec(Cfg *c, const std::string &path, const std::string &loc, std::string &err){
    std::ifstream f(path); if(!f){ err="cannot open spec "+path; return false; }
    std::string line; unsigned nextId=1; int nt=0;
    while(std::getline(f,line)){ if(line.empty()) continue;
        std::stringstream ss(line); std::string tag; std::getline(ss,tag,'\t');
        if(tag=="P"){ std::string ty,lit; std::getline(ss,ty,'\t'); std::getline(ss,lit,'\t');
            if(!loc.empty()&&ty.find(loc)==std::string::npos) continue; if(!lit.empty()) c->ents[ty].push_back(lit); }
        else if(tag=="T"){ std::vector<std::pair<int,std::string>> steps; std::string fld; bool ok=true;
            while(std::getline(ss,fld,'\t')){ size_t p=fld.find(':'); if(p==std::string::npos){ok=false;break;}
                std::string cn=fld.substr(0,p),ty=fld.substr(p+1);
                if(!loc.empty()&&ty.find(loc)==std::string::npos){ok=false;break;} steps.push_back({specConn(cn),ty}); }
            if(!ok||steps.size()<2) continue;
            Template t; t.id=100000+nextId++; t.name="spec_t"+std::to_string(t.id);
            for(size_t i=0;i<steps.size();i++){ Step st; st.conn=(i==0?NONE:steps[i].first); st.entity=steps[i].second; t.steps.push_back(st); }
            c->templates.push_back(std::move(t)); nt++; }
    }
    if(nt==0){ err="spec: no templates parsed (locale '"+loc+"')"; return false; } return true;
}
bool cfgCompile(Cfg *c, std::string &err){
    std::set<std::string> used; for(auto &t:c->templates) for(auto &s:t.steps) used.insert(s.entity);
    std::vector<const char*> ex; std::vector<unsigned> fl,id; std::vector<size_t> lens;
    for(auto &en:used){ if(!c->ents.count(en)){ err="component missing at compile: "+en; return false; }
        int ti=(int)c->typeName.size(); c->typeIdx[en]=ti; c->typeName.push_back(en);
        for(auto &w:c->ents[en]){ ex.push_back(w.c_str()); fl.push_back(0/*no SOM*/); id.push_back((unsigned)c->id2type.size());
            lens.push_back(w.size()); c->id2type.push_back(ti); c->litLen.push_back((uint32_t)w.size()); } }
    hs_compile_error_t *e=nullptr;
    if(hs_compile_lit_multi(ex.data(),fl.data(),id.data(),lens.data(),(unsigned)ex.size(),
                            HS_MODE_BLOCK,nullptr,&c->db,&e)!=HS_SUCCESS){ err=e&&e->message?e->message:"compile fail"; if(e)hs_free_compile_error(e); return false; }
    if(hs_alloc_scratch(c->db,&c->scr)!=HS_SUCCESS){ err="scratch alloc fail"; return false; } return true;
}
namespace { struct FT{uint32_t from,to;}; struct SC{ std::vector<std::vector<FT>>*byType; Cfg*c; }; }
static int onMatch(unsigned id,unsigned long long,unsigned long long to,unsigned,void*ctx){
    auto*sc=(SC*)ctx; int t=sc->c->id2type[id]; uint32_t f=(uint32_t)(to-sc->c->litLen[id]);
    (*sc->byType)[t].push_back({f,(uint32_t)to}); return 0; }
int cfgScan(Cfg *c, const char *text, size_t len, std::vector<CfgDet> &out){
    std::string s(text,len); int NT=(int)c->typeName.size(); std::vector<std::vector<FT>> byType(NT);
    SC sc{&byType,c}; hs_scan(c->db,text,(unsigned)len,0,c->scr,onMatch,&sc);
    for(auto &v:byType) std::sort(v.begin(),v.end(),[](const FT&a,const FT&b){return a.from<b.from;});
    auto pack=[](uint32_t a,uint32_t b){return ((uint64_t)a<<32)|b;};
    for(auto &t:c->templates){ int t0=c->typeIdx[t.steps[0].entity];
        std::unordered_set<uint64_t> part; for(auto &ft:byType[t0]) part.insert(pack(ft.from,ft.to));
        for(size_t k=1;k<t.steps.size()&&!part.empty();k++){ int conn=t.steps[k].conn; const std::string&lit=t.steps[k].lit;
            const auto &bs=byType[c->typeIdx[t.steps[k].entity]]; std::unordered_set<uint64_t> nx;
            for(uint64_t v:part){ uint32_t start=(uint32_t)(v>>32),pos=(uint32_t)v; uint32_t amax=(uint32_t)std::min<size_t>(pos+MAXGAP,len);
                auto lo=std::lower_bound(bs.begin(),bs.end(),pos,[](const FT&f,uint32_t p){return f.from<p;});
                for(auto it=lo; it!=bs.end()&&it->from<=amax; ++it) if(connOk(conn,s,pos,it->from,lit)) nx.insert(pack(start,it->to)); }
            part.swap(nx); }
        for(uint64_t v:part){ uint32_t st=(uint32_t)(v>>32),en=(uint32_t)v;
            if(st>0&&isWord((unsigned char)s[st-1])&&isWord((unsigned char)s[st])) continue;
            if(en<len&&isWord((unsigned char)s[en-1])&&isWord((unsigned char)s[en])) continue;
            out.push_back({t.id,st,en}); } }
    std::sort(out.begin(),out.end(),[](const CfgDet&a,const CfgDet&b){return a.from<b.from||(a.from==b.from&&a.to>b.to);});
    return (int)out.size();
}

// ================== Phase 4: real hs_scan composition dispatch ==================
// Copy out a db's CfgMeta (returns false if absent). A copy keeps it immutable + scan-local.
static bool cfgLookupMeta(const hs_database_t *db, CfgMeta &out){
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    auto it=g_cfgReg.find(db); if(it==g_cfgReg.end()) return false; out=it->second; return true;
}
namespace { struct CollectCtx { const CfgMeta *meta; std::vector<std::vector<FT>> *byType; }; }
// Internal collection callback (C ABI). Records each component match's (from,to) span by type for the
// post-scan join. It does NOT forward to the user callback: internal component-literal pattern ids
// occupy [0,N) and would collide numerically with composite ids on the shared callback, so they are
// kept PRIVATE. The user callback receives ONLY composite matches (compose ids), emitted post-scan.
// (If base-entity emission is ever wanted it must use normalized entity ids, not raw literal ids.)
// Always returns 0: collection must complete for the join to be correct (never halts mid-scan).
extern "C" int cfg_collect_cb(unsigned id, unsigned long long from,
                              unsigned long long to, unsigned flags, void *ctx){
    CollectCtx *cc=(CollectCtx*)ctx; const CfgMeta *m=cc->meta; (void)from; (void)flags;
    // Bounds guard: a mismatched sidecar (attached to the wrong DB) could yield an id outside the
    // metadata tables. Skip it rather than index out of range. Also guards typeIdx range for byType.
    if(id>=m->id2type.size()||id>=m->litLen.size()) return 0;
    int ty=m->id2type[id];
    if(ty<0||ty>=(int)cc->byType->size()) return 0;
    (*cc->byType)[ty].push_back({(uint32_t)(to-m->litLen[id]),(uint32_t)to});
    return 0;
}
// Real-hs_scan dispatcher (armed into cfg_dispatch_hook). Runs the ordinary component scan through
// hs_scan_i, then position-joins the collected spans and emits composites to the SAME user callback.
extern "C" hs_error_t cfg_scan_dispatch(const hs_database_t *db, const char *data,
                                        unsigned length, unsigned flags,
                                        hs_scratch_t *scratch,
                                        match_event_handler onEvent, void *userCtx){
    CfgMeta meta;
    if(!cfgLookupMeta(db,meta)) return HS_INVALID; // gate bit set but metadata absent: never silent-drop
    int NT=(int)meta.typeName.size();
    std::vector<std::vector<FT>> byType(NT);
    CollectCtx cc{&meta,&byType};
    hs_error_t rv=hs_scan_i(db,data,length,flags,scratch,cfg_collect_cb,&cc);
    if(rv!=HS_SUCCESS) return rv; // real scan error -> no composites emitted (collection never self-halts)
    // ---- position-join (close-to-verbatim from cfgScan / vs_chain_bench :350-360) ----
    std::string s(data,length); size_t len=length;
    for(auto &v:byType) std::sort(v.begin(),v.end(),[](const FT&a,const FT&b){return a.from<b.from;});
    auto pack=[](uint32_t a,uint32_t b){return ((uint64_t)a<<32)|b;};
    std::vector<CfgDet> out;
    for(auto &t:meta.templates){ auto i0=meta.typeIdx.find(t.steps[0].entity); if(i0==meta.typeIdx.end()) continue; int t0=i0->second;
        std::unordered_set<uint64_t> part; for(auto &ft:byType[t0]) part.insert(pack(ft.from,ft.to));
        for(size_t k=1;k<t.steps.size()&&!part.empty();k++){ int conn=t.steps[k].conn; const std::string&lit=t.steps[k].lit;
            auto ik=meta.typeIdx.find(t.steps[k].entity); if(ik==meta.typeIdx.end()){ part.clear(); break; }
            const auto &bs=byType[ik->second]; std::unordered_set<uint64_t> nx;
            for(uint64_t v:part){ uint32_t start=(uint32_t)(v>>32),pos=(uint32_t)v; uint32_t amax=(uint32_t)std::min<size_t>(pos+MAXGAP,len);
                auto lo=std::lower_bound(bs.begin(),bs.end(),pos,[](const FT&f,uint32_t p){return f.from<p;});
                for(auto it=lo; it!=bs.end()&&it->from<=amax; ++it) if(connOk(conn,s,pos,it->from,lit)) nx.insert(pack(start,it->to)); }
            part.swap(nx); }
        for(uint64_t v:part){ uint32_t st=(uint32_t)(v>>32),en=(uint32_t)v;
            if(st>0&&isWord((unsigned char)s[st-1])&&isWord((unsigned char)s[st])) continue;
            if(en<len&&isWord((unsigned char)s[en-1])&&isWord((unsigned char)s[en])) continue;
            out.push_back({t.id,st,en}); } }
    std::sort(out.begin(),out.end(),[](const CfgDet&a,const CfgDet&b){return a.from<b.from||(a.from==b.from&&a.to>b.to);});
    for(auto &d:out){ int r=onEvent?onEvent(d.id,(unsigned long long)d.from,(unsigned long long)d.to,0,userCtx):0;
        if(r!=0) return HS_SCAN_TERMINATED; } // composite callback early-return honored
    return HS_SUCCESS;
}
// Arm hooks + move CfgMeta into the side-table keyed by db. Both hooks resolve to symbols in this
// (hs_compile) TU; they stay NULL until a CFG DB exists, so hs_runtime links standalone.
static void cfgRegisterMeta(const hs_database_t *db, CfgMeta &&m){
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    g_cfgReg[db]=std::move(m);
    cfg_unregister_hook=&cfg_unregister_impl;
    cfg_dispatch_hook=&cfg_scan_dispatch;
}
hs_database *cfgFinalizeForScan(Cfg *c){
    if(!c->db) return nullptr;
    CfgMeta m; m.id2type=c->id2type; m.litLen=c->litLen; m.typeName=c->typeName; m.typeIdx=c->typeIdx; m.templates=c->templates;
    ((hs_database_t*)c->db)->reserved0 |= HS_DB_CFG_FLAG; // gate bit (excluded from CRC; survives serialize)
    cfgRegisterMeta(c->db,std::move(m));
    return c->db;
}
hs_scratch *cfgScratch(Cfg *c){ return c->scr; }

// ---- Phase 5: compile-path front-door (called from hs_compile via HS_FLAG_GRAMMAR_REF) ----
// Split "path.spec[:locale]" into path + optional locale filter.
static void splitSpecLocale(const std::string &e, std::string &path, std::string &locale){
    size_t sp=e.find(".spec");
    if(sp!=std::string::npos){ size_t end=sp+5; path=e.substr(0,end); locale=(end<e.size()&&e[end]==':')?e.substr(end+1):""; return; }
    path=e; locale="";
}
bool isComposeGrammar(const char *expr){
    if(!expr) return false;
    std::string e(expr);
    if(e.find(".spec")!=std::string::npos) return true;      // .spec front-door
    std::ifstream f(e); if(!f) return false;                 // .hsg with a `compose` directive
    std::string l; while(std::getline(f,l)){ std::string t=trim(l); if(t.rfind("compose ",0)==0) return true; }
    return false;
}
int compileComposeGrammar(const char *expr, unsigned mode, hs_database **db, std::string &err){
    if(!expr||!db){ err="compose: null argument"; return HS_INVALID; }
    if(mode!=HS_MODE_BLOCK){ err="compose grammar requires HS_MODE_BLOCK"; return HS_INVALID; }
    std::string e(expr); Cfg *c=cfgCreate(); bool ok=false;
    if(e.find(".spec")!=std::string::npos){
        std::string path,locale; splitSpecLocale(e,path,locale); ok=cfgLoadSpec(c,path,locale,err);
    } else {
        // compose .hsg: lines are `dict <name> <relpath>` (component dictionary, path relative to the
        // .hsg dir) and `compose <id> <name>: ...` (template). Blank/`#` lines ignored.
        std::ifstream f(e); if(!f){ err="compose: cannot open "+e; cfgDestroy(c); return HS_COMPILER_ERROR; }
        std::string dir; size_t sl=e.find_last_of('/'); if(sl!=std::string::npos) dir=e.substr(0,sl+1);
        std::string l; ok=true;
        while(std::getline(f,l)){ std::string t=trim(l); if(t.empty()||t[0]=='#') continue;
            if(t.rfind("dict ",0)==0){ std::istringstream is(t); std::string kw,name,rel; is>>kw>>name>>rel;
                if(name.empty()||rel.empty()){ err="compose: malformed dict line: "+t; ok=false; break; }
                std::string p=(!rel.empty()&&rel[0]=='/')?rel:dir+rel;
                if(!cfgAddEntityFile(c,name,p,err)){ ok=false; break; } }
            else if(t.rfind("compose ",0)==0){ if(!cfgAddComposeLine(c,t,err)){ ok=false; break; } }
            else { err="compose: unknown directive: "+t; ok=false; break; } }
    }
    if(!ok){ cfgDestroy(c); return HS_COMPILER_ERROR; }
    if(!cfgCompile(c,err)){ cfgDestroy(c); return HS_COMPILER_ERROR; }
    hs_database *d=cfgFinalizeForScan(c); // sets gate bit + registers metadata keyed by d
    c->db=nullptr;                        // release DB ownership to caller (Cfg dtor frees only scratch)
    cfgDestroy(c);
    *db=d;
    return HS_SUCCESS;
}

// ---- Phase 6: sidecar keyed by a real hs_database (works with the public serialize + hs_scan) ----
static void writeMeta(std::ostream &sc, const CfgMeta &m){
    sc<<"CFGSIDECAR 1\n";
    sc<<"TYPES "<<m.typeName.size()<<"\n"; for(auto&t:m.typeName) sc<<t<<"\n";
    sc<<"ID2TYPE "<<m.id2type.size()<<"\n"; for(size_t i=0;i<m.id2type.size();i++) sc<<m.id2type[i]<<" "<<m.litLen[i]<<"\n";
    sc<<"TEMPLATES "<<m.templates.size()<<"\n";
    for(auto&t:m.templates){ sc<<t.id<<" "<<t.name<<" "<<t.steps.size()<<"\n";
        for(auto&s:t.steps) sc<<s.conn<<"\t"<<s.entity<<"\t"<<s.lit<<"\n"; }
}
static bool readMeta(std::istream &sc, CfgMeta &m, std::string &err){
    std::string tok; int ver=0; sc>>tok>>ver; if(tok!="CFGSIDECAR"){ err="sidecar: bad magic"; return false; }
    sc>>tok; size_t nt=0; sc>>nt; { std::string junk; std::getline(sc,junk); }
    for(size_t i=0;i<nt;i++){ std::string t; std::getline(sc,t); m.typeName.push_back(t); m.typeIdx[t]=(int)i; }
    sc>>tok; size_t nid=0; sc>>nid; for(size_t i=0;i<nid;i++){ int ty; uint32_t ll; sc>>ty>>ll; m.id2type.push_back(ty); m.litLen.push_back(ll); }
    sc>>tok; size_t ntpl=0; sc>>ntpl; { std::string junk; std::getline(sc,junk); }
    for(size_t i=0;i<ntpl;i++){ Template t; std::string hdr; std::getline(sc,hdr); std::istringstream hs(hdr);
        size_t nsteps=0; hs>>t.id>>t.name>>nsteps;
        for(size_t k=0;k<nsteps;k++){ std::string sl; std::getline(sc,sl); std::istringstream ls(sl);
            Step st; std::string connS,ent,lit; std::getline(ls,connS,'\t'); std::getline(ls,ent,'\t'); std::getline(ls,lit,'\t');
            st.conn=std::atoi(connS.c_str()); st.entity=ent; st.lit=lit; t.steps.push_back(st); }
        m.templates.push_back(std::move(t)); }
    return true;
}
bool cfgSerializeSidecar(const hs_database *db, const std::string &sidecarPath, std::string &err){
    CfgMeta m; if(!cfgLookupMeta(db,m)){ err="serialize sidecar: db has no composition metadata"; return false; }
    std::ofstream sc(sidecarPath); if(!sc){ err="serialize sidecar: cannot open "+sidecarPath; return false; }
    writeMeta(sc,m); return true;
}
int cfgAttachSidecar(hs_database *db, const std::string &sidecarPath, std::string &err){
    if(!db){ err="attach: null db"; return HS_INVALID; }
    std::ifstream sc(sidecarPath); if(!sc){ err="attach: cannot open sidecar "+sidecarPath; return HS_INVALID; }
    CfgMeta m; if(!readMeta(sc,m,err)) return HS_INVALID;
    ((hs_database_t*)db)->reserved0 |= HS_DB_CFG_FLAG; // ensure gate bit (normally already set from serialized header)
    cfgRegisterMeta(db,std::move(m));
    return HS_SUCCESS;
}
// Convention: sidecar path = "<dbPath>.cfgmeta" (the db.hsdb / db.hsdb.cfgmeta file pair).
bool cfgWriteSidecarBeside(const hs_database *db, const std::string &dbPath, std::string &err){
    return cfgSerializeSidecar(db, dbPath + ".cfgmeta", err);
}
int cfgAttachSidecarBeside(hs_database *db, const std::string &dbPath, std::string &err){
    return cfgAttachSidecar(db, dbPath + ".cfgmeta", err);
}

// ---- serialization sidecar: DB via hs_serialize_database; templates + id2type + litLen + typeName as text ----
bool cfgSerialize(Cfg *c, const std::string &dbPath, const std::string &sidecarPath, std::string &err){
    if(!c->db){ err="serialize: not compiled"; return false; }
    char *bytes=nullptr; size_t blen=0;
    if(hs_serialize_database(c->db,&bytes,&blen)!=HS_SUCCESS){ err="hs_serialize_database failed"; return false; }
    { std::ofstream o(dbPath,std::ios::binary); o.write(bytes,(std::streamsize)blen); } free(bytes);
    std::ofstream sc(sidecarPath);
    sc<<"CFGSIDECAR 1\n";
    sc<<"TYPES "<<c->typeName.size()<<"\n"; for(auto&t:c->typeName) sc<<t<<"\n";
    sc<<"ID2TYPE "<<c->id2type.size()<<"\n"; for(size_t i=0;i<c->id2type.size();i++) sc<<c->id2type[i]<<" "<<c->litLen[i]<<"\n";
    sc<<"TEMPLATES "<<c->templates.size()<<"\n";
    for(auto&t:c->templates){ sc<<t.id<<" "<<t.name<<" "<<t.steps.size()<<"\n";
        for(auto&s:t.steps) sc<<s.conn<<"\t"<<s.entity<<"\t"<<s.lit<<"\n"; }
    return true;
}
Cfg *cfgDeserialize(const std::string &dbPath, const std::string &sidecarPath, std::string &err){
    std::ifstream df(dbPath,std::ios::binary); if(!df){ err="open db "+dbPath; return nullptr; }
    std::string bytes((std::istreambuf_iterator<char>(df)),std::istreambuf_iterator<char>());
    Cfg *c=new Cfg();
    if(hs_deserialize_database(bytes.data(),bytes.size(),&c->db)!=HS_SUCCESS){ err="hs_deserialize_database failed"; delete c; return nullptr; }
    if(hs_alloc_scratch(c->db,&c->scr)!=HS_SUCCESS){ err="scratch alloc fail"; delete c; return nullptr; }
    std::ifstream sc(sidecarPath); if(!sc){ err="open sidecar "+sidecarPath; delete c; return nullptr; }
    std::string tok; sc>>tok; int ver; sc>>ver; // CFGSIDECAR 1
    sc>>tok; size_t nt; sc>>nt; { std::string junk; std::getline(sc,junk); }
    for(size_t i=0;i<nt;i++){ std::string t; std::getline(sc,t); c->typeName.push_back(t); c->typeIdx[t]=(int)i; }
    sc>>tok; size_t nid; sc>>nid; for(size_t i=0;i<nid;i++){ int ty; uint32_t ll; sc>>ty>>ll; c->id2type.push_back(ty); c->litLen.push_back(ll); }
    sc>>tok; size_t ntpl; sc>>ntpl; { std::string junk; std::getline(sc,junk); }
    for(size_t i=0;i<ntpl;i++){ Template t; std::string hdr; std::getline(sc,hdr); std::istringstream hs(hdr);
        size_t nsteps; hs>>t.id>>t.name>>nsteps;
        for(size_t k=0;k<nsteps;k++){ std::string sl; std::getline(sc,sl); std::istringstream ls(sl);
            Step st; std::string connS,ent,lit; std::getline(ls,connS,'\t'); std::getline(ls,ent,'\t'); std::getline(ls,lit,'\t');
            st.conn=std::atoi(connS.c_str()); st.entity=ent; st.lit=lit; t.steps.push_back(st); }
        c->templates.push_back(std::move(t)); }
    return c;
}

} // namespace grammar
} // namespace ue2
