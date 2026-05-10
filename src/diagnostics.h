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
 */
void publish_diagnostics(const char *uri, const ParseResult *r);
