/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include "parser.h"

/* ── Diagnostic accumulation ─────────────────────────────────────────────── */

void push_diagnostic(ParseResult *r, LspRange range, int severity,
                     const char *msg);

/* ── Dependency reference tracking ──────────────────────────────────────── *
 *
 * Called from grammar.y semantic actions to record each dep_ref seen during
 * parsing.  Raw refs are transferred to ParseResult via dep_refs_transfer()
 * and later checked by revalidate_dep_refs() in the server once all open
 * documents are available.  dep_refs_reset() must be called at the start of
 * each parse() invocation and free_dep_refs() after validation is complete.
 */

void push_dep_ref(ParseResult *r, int bang_count, const char *path,
                  const char **scope, int scope_n,
                  LspPos start, LspPos end);

void dep_refs_reset(void);
void validate_dep_refs(const DocSymbol *syms, int nsym, ParseResult *r);
void dep_refs_transfer(ParseResult *r);
void free_dep_refs(void);

/*
 * Re-run dep-ref validation for r using its stored raw_dep_refs.
 * Trims dep-validation diagnostics (from r->dep_diag_start) and def_links,
 * then re-validates against r->doc_symbols plus the extra symbol pools.
 * Only absolute (bang_count == 0) references are searched in extra pools.
 */
void revalidate_dep_refs(ParseResult *r,
                         const DocSymbol * const *extra_pools,
                         const int *extra_counts,
                         const char * const *extra_uris,
                         int num_extra);

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

void publish_diagnostics(const char *uri, const ParseResult *r);
