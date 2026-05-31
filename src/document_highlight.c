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

#include "document_highlight.h"
#include "document_symbol.h"
#include "grammar.tab.h"
#include <string.h>

/* Document highlight resolves the cursor to a single target task and reports
 * every same-document occurrence of that task: its declaration as Write
 * (kind 3) and each dependency that resolves to it as Read (kind 2).  The
 * cursor → target resolution (which triggers from both declaration and
 * reference sites) runs in the handler against the pinned snapshot's
 * ProjectNode tree; this file walks that same tree to collect the
 * occurrences, mirroring references.c but emitting highlight kinds instead
 * of cross-file Locations. */

static void push_highlight(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                           LspRange range, int kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "range", range_json(doc, range));
    yyjson_mut_obj_add_uint(doc, obj, "kind", (uint64_t)kind);
    yyjson_mut_arr_add_val(arr, obj);
}

/* A dependency's source_range spans the whole reference (leading bangs and
 * every dotted segment); the highlight, like the original implementation's
 * goldens, covers only the identifier token that names the resolved target —
 * i.e. the path's final segment.  Return the last TK_IDENT token whose start
 * falls within @p range. */
static int last_ident_in_range(const TokenSpan *tokens, int num_tokens,
                               LspRange range, LspRange *out) {
    int found = 0;
    for (int t = 0; t < num_tokens; t++) {
        if (tokens[t].token_kind != TK_IDENT) continue;
        if (pos_cmp(tokens[t].start, range.start) < 0) continue;
        if (pos_cmp(tokens[t].start, range.end) > 0) continue;
        out->start = tokens[t].start;
        out->end   = tokens[t].end;
        found = 1;
    }
    return found;
}

/* Depth-first walk of the project tree, pushing a Read highlight for each
 * dependency that (a) is declared in the current document and (b) resolves to
 * @p wanted.  Cross-file dependencies are skipped: highlight ranges are
 * doc-local, so only references living in @p doc_uri can be reported. */
static void collect_read_highlights(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                    ProjectNode *node, const ProjectNode *wanted,
                                    ProjectNode *project_root,
                                    const char *doc_uri,
                                    const TokenSpan *tokens, int num_tokens) {
    if (!node) return;
    if (node->source_uri && strcmp(node->source_uri, doc_uri) == 0) {
        for (int i = 0; i < node->num_dependencies; i++) {
            if (project_dep_resolve(node, i, project_root) != wanted) continue;
            LspRange segment;
            if (last_ident_in_range(tokens, num_tokens,
                                    node->dependencies[i].source_range,
                                    &segment))
                push_highlight(doc, arr, segment, 2);
        }
    }
    for (int i = 0; i < node->num_children; i++)
        collect_read_highlights(doc, arr, node->children[i], wanted,
                                project_root, doc_uri, tokens, num_tokens);
}

yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    ProjectNode *project_root,
    const ProjectNode *wanted,
    const char *doc_uri,
    const TokenSpan *tokens, int num_tokens) {

    if (!project_root || !wanted || !doc_uri) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    /* Write: the declaration itself, only when it lives in this document. */
    if (wanted->source_uri && strcmp(wanted->source_uri, doc_uri) == 0)
        push_highlight(doc, arr, wanted->selection_range, 3);

    /* Read: every in-document dependency resolving to the target. */
    collect_read_highlights(doc, arr, project_root, wanted, project_root,
                            doc_uri, tokens, num_tokens);

    return arr;
}
