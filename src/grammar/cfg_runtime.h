/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cfg_runtime.h - INTERNAL glue between the C runtime (runtime.c / database.c, in hs_exec)
 * and the C++ CFG composition engine (cfg_compose.cpp, in hs_compile).
 *
 * Not an installed/public header. cfg* names never appear in hs.h/hs_common.h/hs_compile.h/
 * hs_runtime.h. The coupling is deliberately WEAK (function pointers) so the standalone
 * hs_runtime library links with no reference to any hs_compile symbol: the hooks stay NULL
 * unless a CFG database is actually compiled (which requires the compile side to be present).
 */
#ifndef GRAMMAR_CFG_RUNTIME_H
#define GRAMMAR_CFG_RUNTIME_H

#include "hs_common.h"  /* hs_database_t, hs_error_t */
#include "hs_runtime.h" /* hs_scratch_t, match_event_handler */

#ifdef __cplusplus
extern "C" {
#endif

/* CFG-present gate bit, stored in hs_database::reserved0 (which is serialized but NOT
 * covered by the bytecode CRC, so setting it needs no CRC recompute). Non-CFG DBs: bit=0. */
#define HS_DB_CFG_FLAG 0x00000001u

/* Weak hooks, defined in runtime.c (hs_exec), set by the compile side on first CFG register. */
typedef hs_error_t (*cfg_dispatch_fn)(const hs_database_t *db, const char *data,
                                      unsigned length, unsigned flags,
                                      hs_scratch_t *scratch,
                                      match_event_handler onEvent, void *userCtx);
typedef void (*cfg_unregister_fn)(const void *db);

extern cfg_dispatch_fn cfg_dispatch_hook;
extern cfg_unregister_fn cfg_unregister_hook;

/* Original scan body, renamed in Phase 3. Internal external-linkage (not HS_PUBLIC_API).
 * The CFG dispatcher calls this to run the ordinary component scan without re-entering the gate. */
hs_error_t hs_scan_i(const hs_database_t *db, const char *data, unsigned length,
                     unsigned flags, hs_scratch_t *scratch,
                     match_event_handler onEvent, void *userCtx);

#ifdef __cplusplus
}
#endif
#endif /* GRAMMAR_CFG_RUNTIME_H */
