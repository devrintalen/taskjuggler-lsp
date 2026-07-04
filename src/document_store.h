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

/* The live document store: the mutable docs[] slot array the coordinator
 * thread owns, plus the slot lifecycle helpers (find / alloc / install a
 * parse / free).  Everything here is guarded by docs_mutex; query workers
 * never touch it — they read the immutable snapshots pinned into their
 * query_context instead (see query_context.h).
 *
 * MAX_DOCS (the slot count bounding docs[] and every per-snapshot array
 * built from it) is defined in query_context.h, since the
 * workspace_snapshot arrays share the same bound. */

#pragma once

#include "parser.h"
#include "query_context.h"   /* MAX_DOCS, doc_snapshot */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/** Document store slot.  Each slot owns the parse-derived state directly
 *  (per-kind synthetic roots, tokens, include filenames); parse() returns
 *  a transient ParseOutput whose fields get moved here by doc_install_parse(). */
typedef struct Document {
    char        *uri;               /**< owned canonical file:// URI */
    char        *text;              /**< mutable working copy; the snapshot holds its own parsed copy */
    _Atomic uint64_t doc_version;   /**< monotonic stamp counter; the next parse stamps the value, then ++ */
    int          in_use;            /**< 1 when this slot holds a tracked document */
    int          disk_only;         /**< 1 for a background (non-editor) document */
    int          is_cc_root;        /**< 1 when this doc is named directly in compile_commands.json */

    /* Immutable parse output.  `snap` is the current parse's doc_snapshot
     * (NULL before the first parse — use it as the "has-parse" sentinel);
     * `prev_snap` is the one immediately before it, retained so a
     * semanticTokens/delta request can diff against the version the client
     * last held.  Both are refcounted: a published workspace_snapshot also
     * holds refs, so a snapshot outlives any in-flight query reading it.
     * Each is released and rotated by doc_install_parse(). */
    doc_snapshot *snap;             /**< current parse output; see comment above */
    doc_snapshot *prev_snap;        /**< previous parse output retained for delta; see above */

    /* Prefixes applied to this Document by the includer's `include` block,
     * one per kind.  Populated by follow_includes() from the includer's
     * captured IncludeRef when this file is pulled in; stay NULL on a
     * canonical .tjp or on orphan .tji files in a .tji-only workspace.
     * Captured into each workspace_snapshot's ws_doc at build time. */
    char        *task_prefix;       /**< owned task-namespace prefix; may be NULL */
    char        *account_prefix;    /**< owned account-namespace prefix; may be NULL */
    char        *report_prefix;     /**< owned report-namespace prefix; may be NULL */
    char        *resource_prefix;   /**< owned resource-namespace prefix; may be NULL */

    /* Resolved file:// URIs of every `include` directive in this doc,
     * recorded by follow_includes() at parse time.  Owned by the
     * Document; cleared at the top of each follow_includes() run and
     * freed by doc_free().  Lets build_workspace_snapshot() walk the
     * include graph without re-parsing or threading state through the
     * load pipeline. */
    char       **included_uris;     /**< owned array; see comment above */
    int          num_included_uris; /**< number of valid entries in `included_uris` */
    int          included_uris_cap; /**< allocated capacity of `included_uris` */
} Document;

/** Array of all tracked document slots (editor-managed and disk-only). */
extern Document docs[MAX_DOCS];

/** Serializes every read/write of docs[] — slots, their fields, and the
 *  global trees built from them. */
extern pthread_mutex_t docs_mutex;

/**
 * Zero every slot's in_use flag.  Called once at startup before the first
 * message is processed.
 */
void docstore_init(void);

/**
 * Find the in-use Document whose URI matches @p uri, comparing first by
 * exact string then by canonical (normalized) form.
 *
 * @param uri  The file:// URI to search for; may be un-normalized.
 * @return     Pointer into docs[] on success, NULL when not found.
 */
Document *doc_find(const char *uri);

/**
 * Allocate a fresh docs[] slot for @p uri, storing a normalized copy of
 * the URI and setting in_use to 1.
 *
 * @param uri  The file:// URI to assign to the new slot.
 * @return     Pointer to the newly allocated slot, or NULL when all slots
 *             are occupied or normalization fails.
 */
Document *doc_alloc(const char *uri);

/**
 * Release both doc_snapshots held by @p d (the live store's refs), nulling
 * each so the slot is reusable.  A snapshot only frees once any
 * workspace_snapshot and in-flight query also release their refs.
 *
 * @param d  Document whose snap and prev_snap fields are to be released.
 */
void doc_clear_parse_state(Document *d);

/**
 * Build a fresh doc_snapshot from @p po (moving its tree and token spans in)
 * and rotate it onto @p d: the outgoing current snapshot becomes prev_snap
 * (replacing the one before it), so a semanticTokens/delta request can still
 * diff against the version the client last held.  Includes are not part of
 * the snapshot — callers consume them via follow_includes() before calling
 * here; parse_output_free() releases the leftover include array and shell.
 *
 * @param d   Document slot to update.
 * @param po  Parse output to install; may be NULL (leaves snap as NULL).
 */
void doc_install_parse(Document *d, ParseOutput *po);

/**
 * Free all heap memory owned by @p d and zero the slot so it can be reused.
 *
 * @param d  Document slot to free; must be in-use.
 */
void doc_free(Document *d);

/**
 * Read the entire contents of the file at @p path into a newly allocated
 * null-terminated buffer.
 *
 * @param path  Filesystem path of the file to read.
 * @return      Freshly allocated string containing the file contents, or
 *              NULL on any I/O error or allocation failure.
 */
char *read_file(const char *path);
