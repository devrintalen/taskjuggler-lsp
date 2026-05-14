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

/* See doc/modules/definition.rst for the module overview. */

#include "definition.h"
#include "document_symbol.h"

/**
 * Test whether @p p falls within range @p r (both endpoints inclusive).
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

const DefinitionLink *find_def_link_at(const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor) {
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent) {
        for (int j = 0; j < sym->num_def_links; j++) {
            if (pos_in_range(cursor, sym->def_links[j].source))
                return &sym->def_links[j];
        }
    }
    return NULL;
}

yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor, const char *uri) {
    const DefinitionLink *link = find_def_link_at(tokens, num_tokens, cursor);
    if (!link) return NULL;

    const char *target_uri = link->target_uri ? link->target_uri : uri;
    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri", target_uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, link->target->selection_range));
    return location;
}
