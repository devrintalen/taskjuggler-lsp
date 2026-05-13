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

/* See doc/modules/references.rst for the module overview. */

#include "references.h"
#include "document_symbol.h"
#include "grammar.tab.h"
#include <string.h>

/**
 * Test whether @p p falls within range @p r (endpoints inclusive).
 *
 * @param p  Position to test.
 * @param r  Range.
 * @return 1 when @p p is inside @p r, 0 otherwise.
 */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor) {
    const DocSymbol *task = NULL;
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent) {
        if (sym->keyword == KW_TASK
                && pos_in_range(cursor, sym->selection_range)) {
            task = sym;
            break;
        }
    }
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
