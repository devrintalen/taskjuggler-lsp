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

/*
 * references.c — textDocument/references response builder
 *
 * ── Overview ─────────────────────────────────────────────────────────────
 *
 * Find-references is answered from two data structures already in ParseResult:
 *
 *   doc_symbols — the symbol tree; each SK_FUNCTION node has a selection_range
 *                 covering its declaration identifier
 *   def_links   — the DefinitionLink array populated by revalidate_dep_refs();
 *                 each entry maps a reference's source range to the target
 *                 symbol's selection_range
 *
 * At query time, build_references_json():
 *
 *   1. Walks the doc_symbols tree of the cursor document to find the SK_FUNCTION
 *      (task) whose selection_range contains the cursor.  Returns null if none.
 *
 *   2. Scans def_links from EVERY supplied document for entries whose resolved
 *      target points to the task found in step 1.  A def_link in document D
 *      targets the declaration in cursor_uri when:
 *        • link.target_uri == cursor_uri  (explicit cross-file pointer), or
 *        • link.target_uri == NULL and D.uri == cursor_uri (same-file link)
 *      and link.target range-equals the task's selection_range.
 *
 *   3. Returns a JSON array of Location objects, one per matching link.
 *      The array may be empty if no dependency references point to the task
 *      across the whole workspace.
 *
 * ── Trigger constraint ───────────────────────────────────────────────────
 *
 * Only task declaration identifiers (SK_FUNCTION selection_range) trigger a
 * response.  Positioning the cursor on a reference in a depends/precedes
 * clause, on a keyword, or on a non-task symbol returns null.
 */

#include "references.h"
#include "document_symbol.h"
#include <string.h>

/* Returns 1 if position p falls within range r (both endpoints inclusive). */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

/* Returns 1 if ranges a and b have identical start and end positions. */
static int range_eq(LspRange a, LspRange b) {
    return pos_cmp(a.start, b.start) == 0 && pos_cmp(a.end, b.end) == 0;
}

/* Walk the symbol tree depth-first to find the SK_FUNCTION node whose
 * selection_range contains pos.  Returns NULL if no such node exists. */
static const DocSymbol *find_task_at(const DocSymbol *syms, int n, LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION
                && pos_in_range(pos, syms[i].selection_range))
            return &syms[i];
        const DocSymbol *found =
            find_task_at(syms[i].children, syms[i].num_children, pos);
        if (found) return found;
    }
    return NULL;
}

/* Build the JSON array for a textDocument/references response.
 * Finds the task declaration at cursor in cursor_uri, then collects every
 * dep_ref link — across all supplied documents — that resolves to it.
 *
 * doc          — the mutable JSON document that will own the returned value
 * cursor_uri   — URI of the document the cursor is in
 * symbols      — root-level symbol array of the cursor document
 * num_symbols  — number of entries in symbols
 * all_docs     — one entry per open document; all are searched for references
 * num_docs     — number of entries in all_docs
 * cursor       — cursor position from the textDocument/references request
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       const DocSymbol *symbols, int num_symbols,
                                       const RefDocLinks *all_docs, int num_docs,
                                       LspPos cursor) {
    const DocSymbol *task = find_task_at(symbols, num_symbols, cursor);
    if (!task) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int d = 0; d < num_docs; d++) {
        const RefDocLinks *rdl = &all_docs[d];
        for (int i = 0; i < rdl->num_links; i++) {
            const DefinitionLink *link = &rdl->links[i];
            /* Determine the URI of the file that contains the target symbol.
             * target_uri == NULL means the target is in the same file as the
             * reference (i.e. rdl->uri). */
            const char *effective_target_uri =
                link->target_uri ? link->target_uri : rdl->uri;
            if (strcmp(effective_target_uri, cursor_uri) != 0) continue;
            if (!range_eq(link->target, task->selection_range))  continue;

            yyjson_mut_val *location = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, location, "uri", rdl->uri);
            yyjson_mut_obj_add_val(doc, location, "range",
                                   range_json(doc, link->source));
            yyjson_mut_arr_add_val(arr, location);
        }
    }
    return arr;
}
