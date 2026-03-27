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
 *   1. Walks the doc_symbols tree to find the SK_FUNCTION (task) whose
 *      selection_range contains the cursor.  Returns null if none is found.
 *
 *   2. Scans def_links for all entries whose target matches the found
 *      symbol's selection_range.
 *
 *   3. Returns a JSON array of Location objects, one per matching link.
 *      The array may be empty if no dependency references point to the task.
 *
 * ── Trigger constraint ───────────────────────────────────────────────────
 *
 * Only task declaration identifiers (SK_FUNCTION selection_range) trigger a
 * response.  Positioning the cursor on a reference in a depends/precedes
 * clause, on a keyword, or on a non-task symbol returns null.
 */

#include "references.h"
#include "document_symbol.h"

static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

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

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const DefinitionLink *links, int num_links,
                                       const DocSymbol *symbols, int num_symbols,
                                       LspPos cursor, const char *uri) {
    const DocSymbol *task = find_task_at(symbols, num_symbols, cursor);
    if (!task) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < num_links; i++) {
        if (range_eq(links[i].target, task->selection_range)) {
            yyjson_mut_val *location = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, location, "uri", uri);
            yyjson_mut_obj_add_val(doc, location, "range",
                                   range_json(doc, links[i].source));
            yyjson_mut_arr_add_val(arr, location);
        }
    }
    return arr;
}
