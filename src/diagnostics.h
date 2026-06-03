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
 * A single error or warning to be reported to the editor.  @p message is
 * owned (heap-allocated) by whoever holds the Diagnostic; @p source is a
 * borrowed static string (e.g. "tj3") naming the producer, or NULL.
 */
struct Diagnostic {
    LspRange    range;
    int         severity;
    char       *message;     /**< owned */
    const char *source;      /**< borrowed static literal; may be NULL */
};

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

/**
 * Send a textDocument/publishDiagnostics notification for @p uri carrying
 * @p count diagnostics from @p diags (which may be NULL when @p count is 0,
 * clearing markers for the URI).
 *
 * @param uri    Document URI the diagnostics apply to.
 * @param diags  Borrowed array of @p count diagnostics (not freed here).
 * @param count  Number of diagnostics.
 */
void publish_diagnostics_list(const char *uri, const Diagnostic *diags, int count);

/**
 * Send a textDocument/publishDiagnostics notification for @p uri with an
 * empty diagnostics array (clears any previous markers for the URI).
 *
 * @param uri  Document URI whose diagnostics are being cleared.
 */
void publish_diagnostics(const char *uri);

/* ── Multi-source diagnostic aggregation (diag_set) ──────────────────────── *
 *
 * publishDiagnostics replaces *all* diagnostics for a URI in one message, so
 * independent producers cannot publish separately — their diagnostics must be
 * merged per-URI first.  A diag_set collects diagnostics keyed by URI from any
 * number of sources, then flushes the merged result in one publish per URI.
 */

/** Opaque ordered map URI -> growable Diagnostic[]. */
typedef struct diag_set diag_set;

/** Allocate an empty diag_set. */
diag_set *diag_set_new(void);

/** Append @p d to the entry for @p uri (created on first use).  Takes
 *  ownership of @p d.message; @p uri is copied. */
void diag_set_add(diag_set *s, const char *uri, Diagnostic d);

/** Free @p s and every diagnostic message it owns.  NULL-safe. */
void diag_set_free(diag_set *s);

/**
 * Publish @p current: one notification per URI it holds.  Additionally emit an
 * empty array for every URI present in @p previous but absent from @p current,
 * so diagnostics that went away are cleared.  Either argument may be NULL.
 */
void diag_set_publish(const diag_set *current, const diag_set *previous);

/* ── Server-level diagnostics ────────────────────────────────────────────── */

struct workspace_snapshot;
struct ws_project;

/**
 * Degradation status of the workspace's compile_commands.json, published on the
 * workspace_snapshot.  CC_STATUS_OK means a usable file drove the snapshot and
 * cross-file features are fully enabled; the other two states load no project
 * closures, so every editor file is parsed stand-alone and gets a per-file
 * warning (see diag_collect_cc_missing).
 */
typedef enum {
    CC_STATUS_OK = 0,     /**< usable compile_commands.json */
    CC_STATUS_MISSING,    /**< no workspace root, or the file is absent */
    CC_STATUS_MALFORMED,  /**< present but not valid JSON / wrong schema */
} cc_status;

/**
 * Collect the "Missing/Malformed compile_commands.json" warnings for @p proj
 * into @p out.  A no-op unless @p ws has a non-OK cc_status: in that state no
 * project closures are loaded and every editor file is parsed stand-alone, so
 * each editor-managed member document of @p proj gets a warning — one per
 * `include` directive in a .tjp (on its KW_INCLUDE token), or one at the top of
 * a stand-alone .tji.  The message reflects whether the file is missing or
 * malformed.  Added to the same diag_set the tj3 runner fills so the two
 * sources merge per URI.
 *
 * Forward-declared struct pointers keep this header free of the snapshot
 * layer; callers (the diagnostics worker) include query_context.h.
 */
void diag_collect_cc_missing(const struct workspace_snapshot *ws,
                             const struct ws_project *proj, diag_set *out);
