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

/** @file */

#pragma once

#include "parser.h"

/* ── Diagnostic severity ─────────────────────────────────────────────────── */

/** LSP DiagnosticSeverity for hard errors. */
#define DIAG_ERROR   1
/** LSP DiagnosticSeverity for warnings. */
#define DIAG_WARNING 2

/**
 * A single error or warning to be reported to the editor.
 *
 * severity uses the LSP DiagnosticSeverity values (DIAG_ERROR=1, DIAG_WARNING=2).
 *
 * Two sources of diagnostics are stored together in ParseResult, distinguished
 * by dep_diag_start:
 *   [0 .. dep_diag_start-1]  ->  permanent diagnostics emitted during parse()
 *                                (syntax errors, in-file dep resolution errors)
 *   [dep_diag_start .. end]  ->  cross-file dep diagnostics, cleared and
 *                                regenerated on every revalidation cycle
 *
 * Example TJP input with an unresolved dependency:
 *
 *   task gui "GUI" {
 *       depends missing_task       <- line 1, characters 16-28
 *   }
 *
 * Produces:
 *
 *   Diagnostic {
 *     .range    = { {1,16}, {1,28} },
 *     .severity = DIAG_ERROR,
 *     .message  = "Unknown task: missing_task",
 *   }
 */
struct Diagnostic {
    LspRange  range;     /**< source range the diagnostic applies to */
    int       severity;  /**< DIAG_ERROR or DIAG_WARNING */
    char     *message;   /**< heap-allocated */
};

/* ── Diagnostic accumulation ─────────────────────────────────────────────── */

/**
 * Append a Diagnostic to @p r->diagnostics.  Used by the grammar/parser and
 * by cross-file resolution to report errors and warnings that will later be
 * forwarded to the editor via publish_diagnostics().
 *
 * @param r         Parse result whose diagnostics array is appended to.
 * @param range     Source range the diagnostic applies to.
 * @param severity  DIAG_ERROR or DIAG_WARNING.
 * @param msg       Diagnostic message; copied into a fresh heap allocation.
 */
void push_diagnostic(ParseResult *r, LspRange range, int severity,
                     const char *msg);

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

/**
 * Serialise @p r->diagnostics and send a textDocument/publishDiagnostics
 * notification for @p uri to the editor.
 *
 * @param uri  Document URI whose diagnostics are being published.
 * @param r    ParseResult whose diagnostics array is serialised.
 */
void publish_diagnostics(const char *uri, const ParseResult *r);
