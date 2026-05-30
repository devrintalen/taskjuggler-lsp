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
 * A single error or warning to be reported to the editor.  Carried forward
 * from the previous design so that other modules referencing the type still
 * compile, even though diagnostic collection is currently dropped.
 */
struct Diagnostic {
    LspRange  range;
    int       severity;
    char     *message;
};

/* ── LSP publishDiagnostics notification ─────────────────────────────────── *
 *
 * TODO(diagnostics): Diagnostic collection was removed during the tj_node
 * refactor.  publish_diagnostics() is kept as a stub that publishes an
 * empty diagnostics array for @p uri so the client clears any previous
 * markers when documents are reparsed, opened, closed, or renamed.
 *
 * Restore richer behaviour once the global tj_node tree is in place and
 * dep resolution / unresolved-include / syntax-error reporting are
 * reintroduced.
 */

/**
 * Send a textDocument/publishDiagnostics notification for @p uri with an
 * empty diagnostics array.
 *
 * @param uri  Document URI whose diagnostics are being cleared.
 */
void publish_diagnostics(const char *uri);
