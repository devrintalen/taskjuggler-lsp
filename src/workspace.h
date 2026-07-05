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

/* Workspace loading and revalidation: everything between the live document
 * store (document_store.h) and the published immutable workspace_snapshot
 * (query_context.h).  Owns the workspace root and the compile_commands.json
 * cache, loads files from disk and follows their include closures, and
 * rebuilds + publishes the snapshot after every document-changing
 * notification.  Every function here runs on the coordinator thread under
 * docs_mutex; see doc/modules/workspace.rst for the loading pipeline. */

#pragma once

#include "document_store.h"
#include "query_context.h"

/**
 * Record the workspace root from the initialize request's rootUri (decoded
 * to a filesystem path) and derive the compile_commands.json path from it.
 * Both are set at most once; later calls are no-ops.  A NULL or non-string
 * rootUri leaves the server in single-file mode (no root, no cc path).
 *
 * @param root_uri  The rootUri string from initialize params; may be NULL.
 */
void workspace_set_root_from_uri(const char *root_uri);

/** @return The workspace root path, or NULL when no folder is open. */
const char *workspace_root(void);

/** @return The absolute compile_commands.json path, or NULL before initialize. */
const char *workspace_cc_path(void);

/**
 * The currently published immutable workspace snapshot, or NULL until the
 * first revalidation builds one.  Touched only by the coordinator thread;
 * callers that hand it to a worker must ws_acquire() it first.
 *
 * @return Borrowed snapshot pointer; do not release.
 */
workspace_snapshot *workspace_current(void);

/**
 * Load the file at @p path into the document store as a disk-only entry,
 * parse it, and follow its includes.  Does nothing if the URI derived
 * from @p path is already tracked.
 *
 * @param path  Absolute filesystem path of the .tjp/.tji file to load.
 */
void load_file_from_disk(const char *path);

/**
 * Replace @p document's text with @p text and reparse it as a background
 * (disk_only) entry: takes ownership of @p text, marks the slot disk_only,
 * then parses, follows includes (resolved against @p path), and installs the
 * new ParseOutput.
 *
 * @param document  Document slot to update.
 * @param text      New file contents; ownership transfers to @p document.
 * @param path      Filesystem path of the file, for include resolution
 *                  (borrowed; the caller still owns and frees it).
 */
void install_disk_text(Document *document, char *text, const char *path);

/**
 * Resolve each include directive in @p po against @p file_path's directory,
 * load any not-yet-tracked included file from disk, propagate the
 * include's per-kind prefix strings onto the includee's Document slot,
 * and record the resolved URI in the includer's included_uris[] array.
 *
 * @param file_path  Filesystem path of the file that was just parsed.
 * @param po         Parse output from parsing @p file_path; may be NULL.
 */
void follow_includes(const char *file_path, const ParseOutput *po);

/**
 * (Re)load compile_commands.json from the workspace root into docs[],
 * refreshing the cached stat metadata and recording the cc-status (OK,
 * MISSING, or MALFORMED) for the next published snapshot.  A missing file
 * or absent workspace root is treated as a legitimate single-file scenario
 * (reported per-file via diagnostics), not a hard error.
 */
void reload_compile_commands(void);

/**
 * Revalidate the full document store: conditionally reload
 * compile_commands.json, rebuild and publish a fresh workspace snapshot,
 * clear client-side diagnostic markers, and hand the snapshot to the
 * per-project diagnostics worker registry.  Called after every
 * document-mutating notification.
 */
void revalidate_all_docs(void);
