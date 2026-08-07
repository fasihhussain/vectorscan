/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cfg_compose.h - CFG 1-layer composition, compiled INTO libhs (native).
 *
 * Depth-1 composition: a composite entity is an ordered concatenation of FLAT entities joined by a
 * connector-gap. Two front-doors feed one engine: `compose <id> <name>: e1, e2[, "lit"|connector]`
 * (.hsg path) and a `.spec` P/T file. Mechanism = position-join (ported from vs_chain_bench):
 * compile flat components (literals, no SOM; start = to - litLen), scan, group by type, adjacency-join.
 */
#ifndef GRAMMAR_CFG_COMPOSE_H
#define GRAMMAR_CFG_COMPOSE_H

#include <cstddef>
#include <string>
#include <vector>

struct hs_database; // fwd (C tag); real definition in database.h
struct hs_scratch;

namespace ue2 {
namespace grammar {

struct CfgDet { unsigned id; unsigned from; unsigned to; };

class Cfg; // opaque composition handle

Cfg *cfgCreate();
void cfgDestroy(Cfg *);

// Register a flat entity's dictionary (one literal per line file).
bool cfgAddEntityFile(Cfg *, const std::string &entity, const std::string &dictPath, std::string &err);
// .hsg front-door: `compose <id> <name>: e1, e2[, "lit"|connector]`. Enforces depth-1 at parse time.
bool cfgAddComposeLine(Cfg *, const std::string &line, std::string &err);
// spec front-door: parse a P/T .spec file (optionally filtered to one locale substring).
bool cfgLoadSpec(Cfg *, const std::string &specPath, const std::string &localeFilter, std::string &err);
// Compile all tracked components into an HS database (literals, no SOM).
bool cfgCompile(Cfg *, std::string &err);
// Scan text, run position-join per template, return composite detections.
int cfgScan(Cfg *, const char *text, size_t len, std::vector<CfgDet> &out);
int cfgTemplateCount(const Cfg *); // structural: templates parsed (no compile needed)

// --- real hs_scan integration (Phase 4/5) ---
// Attach a compiled Cfg's composition metadata to its DB (side-table), set the CFG gate bit, and
// arm the runtime hooks. Returns the DB; the caller then scans with the STANDARD public
// hs_scan(db,...) callback path (no cfgScan). cfgScratch exposes the Cfg's scratch for that scan.
struct hs_database *cfgFinalizeForScan(Cfg *);
struct hs_scratch  *cfgScratch(Cfg *);

// --- compile-path front-door (Phase 5): entered from hs_compile via HS_FLAG_GRAMMAR_REF ---
// True if `expr` names a composition grammar: a ".spec" file (optionally "path.spec:locale"), or a
// ".hsg" file containing a `compose` directive. Distinguishes compose from ordinary import grammars.
bool isComposeGrammar(const char *expr);
// Compile a composition grammar file into a ready hs_database: component literals compiled normally,
// composition metadata attached + gate bit set. The returned DB is owned by the caller (free with
// hs_free_database). Scans go through the STANDARD public hs_scan. mode must be HS_MODE_BLOCK.
// Returns HS_SUCCESS or an hs_error_t; on failure sets err. No cfg* API is needed by the caller.
int compileComposeGrammar(const char *expr, unsigned mode, struct hs_database **db, std::string &err);

// --- serialization sidecar (additive; templates/connectors/component-id map next to the .hsdb) ---
bool cfgSerialize(Cfg *, const std::string &dbPath, const std::string &sidecarPath, std::string &err);
Cfg *cfgDeserialize(const std::string &dbPath, const std::string &sidecarPath, std::string &err);

// --- sidecar for the real hs_scan path (Phase 6), keyed by a compiled/deserialized hs_database ---
// Write the composition metadata (from the side-table) alongside a DB serialized with the PUBLIC
// hs_serialize_database. LIMITATION: hs_deserialize_database(bytes,len) takes no path, so it cannot
// auto-discover the sidecar; after deserialize the caller must call cfgAttachSidecar (documented
// internal attach). The scan itself is the STANDARD public hs_scan.
bool cfgSerializeSidecar(const struct hs_database *db, const std::string &sidecarPath, std::string &err);
int  cfgAttachSidecar(struct hs_database *db, const std::string &sidecarPath, std::string &err);

// Tool/serving-layer convenience for the decided Phase-1 model: the sidecar lives beside the DB at
// "<dbPath>.cfgmeta", so a layer that knows the .hsdb path handles metadata without a separate arg.
// These are NOT public API (internal header only); the end user still scans with plain hs_scan.
bool cfgWriteSidecarBeside(const struct hs_database *db, const std::string &dbPath, std::string &err);
int  cfgAttachSidecarBeside(struct hs_database *db, const std::string &dbPath, std::string &err);

} // namespace grammar
} // namespace ue2
#endif
