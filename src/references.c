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
 *   2. Iterates the target task's ref_links[] to collect all incoming
 *      references (same-document and cross-document).
 *
 *   3. Returns a JSON array of Location objects, one per reference.
 *      The array may be empty if no dependency references point to the task.
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
 * selection_range contains pos.  Uses each symbol's range to skip
 * subtrees that cannot contain pos.  Returns NULL if no match. */
static const DocSymbol *find_task_at(DocSymbol *const *syms, int n, LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (!pos_in_range(pos, syms[i]->range))
            continue;
        if (syms[i]->keyword == KW_TASK
                && pos_in_range(pos, syms[i]->selection_range))
            return syms[i];
        return find_task_at(syms[i]->children, syms[i]->num_children, pos);
    }
    return NULL;
}

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       DocSymbol *const *symbols, int num_symbols,
                                       LspPos cursor) {
    const DocSymbol *task = find_task_at(symbols, num_symbols, cursor);
    if (!task) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < task->num_ref_links; i++) {
        const ReferenceLink *ref = &task->ref_links[i];
        const char *uri = ref->source_uri ? ref->source_uri : cursor_uri;

        yyjson_mut_val *location = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, location, "uri", uri);
        yyjson_mut_obj_add_val(doc, location, "range",
                               range_json(doc, ref->source));
        yyjson_mut_arr_add_val(arr, location);
    }
    return arr;
}
