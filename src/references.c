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
 *   doc_symbols — the symbol tree; each KW_TASK node has a selection_range
 *                 covering its declaration identifier, and each node may carry
 *                 def_links[] pointing to resolved target DocSymbols
 *
 * At query time, build_references_json():
 *
 *   1. Walks the doc_symbols tree of the cursor document to find the KW_TASK
 *      (task) whose selection_range contains the cursor.  Returns null if none.
 *
 *   2. Walks the symbol tree of EVERY supplied document looking for
 *      DefinitionLinks whose target pointer matches the task found in step 1.
 *
 *   3. Returns a JSON array of Location objects, one per matching link.
 *      The array may be empty if no dependency references point to the task
 *      across the whole workspace.
 *
 * ── Trigger constraint ───────────────────────────────────────────────────
 *
 * Only task declaration identifiers (KW_TASK selection_range) trigger a
 * response.  Positioning the cursor on a reference in a depends/precedes
 * clause, on a keyword, or on a non-task symbol returns null.
 */

#include "references.h"
#include "document_symbol.h"
#include "grammar.tab.h"
#include <string.h>

/* Returns 1 if position p falls within range r (both endpoints inclusive). */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

/* Walk the symbol tree depth-first to find the KW_TASK node whose
 * selection_range contains pos.  Returns NULL if no such node exists. */
static const DocSymbol *find_task_at(DocSymbol *const *syms, int n, LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (syms[i]->keyword == KW_TASK
                && pos_in_range(pos, syms[i]->selection_range))
            return syms[i];
        const DocSymbol *found =
            find_task_at(syms[i]->children, syms[i]->num_children, pos);
        if (found) return found;
    }
    return NULL;
}

/* Walk the symbol tree and collect all DefinitionLinks whose target matches
 * the given task pointer.  Appends Location objects to arr. */
static void collect_refs(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                         DocSymbol *const *syms, int n,
                         const DocSymbol *task, const char *doc_uri) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < syms[i]->num_def_links; j++) {
            const DefinitionLink *link = &syms[i]->def_links[j];
            if (link->target != task) continue;

            yyjson_mut_val *location = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, location, "uri", doc_uri);
            yyjson_mut_obj_add_val(doc, location, "range",
                                   range_json(doc, link->source));
            yyjson_mut_arr_add_val(arr, location);
        }
        collect_refs(doc, arr, syms[i]->children, syms[i]->num_children,
                     task, doc_uri);
    }
}

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       DocSymbol *const *symbols, int num_symbols,
                                       const RefDocLinks *all_docs, int num_docs,
                                       LspPos cursor) {
    const DocSymbol *task = find_task_at(symbols, num_symbols, cursor);
    if (!task) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int d = 0; d < num_docs; d++) {
        const RefDocLinks *rdl = &all_docs[d];
        collect_refs(doc, arr, rdl->symbols, rdl->num_symbols,
                     task, rdl->uri);
    }
    return arr;
}
