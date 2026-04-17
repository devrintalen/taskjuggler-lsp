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
 * definition.c — textDocument/definition response builder
 *
 * ── Overview ─────────────────────────────────────────────────────────────
 *
 * Go-to-definition is answered from DefinitionLink arrays stored on each
 * DocSymbol.  Each DefinitionLink records a source range (the reference
 * expression) and a target pointer (the resolved DocSymbol).
 *
 * These links are populated by parser.c:resolve_dep_refs() after every
 * document change for every successfully resolved dependency reference.
 *
 * At query time, build_definition_json() walks the symbol tree looking for a
 * DefinitionLink whose source range contains the cursor and, when found,
 * returns a Location object pointing at the target symbol's selection_range.
 *
 * ── Supported references ─────────────────────────────────────────────────
 *
 * Currently only task dependency references (`depends` / `precedes` paths)
 * produce definition links.  Resource references (`allocate`, `responsible`,
 * `booking`) are not yet tracked.
 */

#include "definition.h"
#include "document_symbol.h"

/* Returns 1 if position p falls within range r (both endpoints inclusive). */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

/* Walk the symbol tree looking for a DefinitionLink whose source range
 * contains the cursor.  Returns a pointer to the matching link, or NULL. */
const DefinitionLink *find_def_link_at(DocSymbol *const *syms, int n,
                                       LspPos cursor) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < syms[i]->num_def_links; j++) {
            if (pos_in_range(cursor, syms[i]->def_links[j].source))
                return &syms[i]->def_links[j];
        }
        const DefinitionLink *found =
            find_def_link_at(syms[i]->children, syms[i]->num_children, cursor);
        if (found) return found;
    }
    return NULL;
}

/* Build a Location JSON object for the go-to-definition response.
 * Walks the symbol tree for a DefinitionLink whose source range contains cursor.
 * Returns NULL if no matching link is found (server will return null to editor).
 *
 * doc         — the mutable JSON document that will own the returned value
 * symbols     — root-level symbol array from the ParseResult
 * num_symbols — number of entries in symbols
 * cursor      — cursor position from the textDocument/definition request
 * uri         — URI of the requesting document, used as fallback target URI
 */
yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       DocSymbol *const *symbols, int num_symbols,
                                       LspPos cursor, const char *uri) {
    const DefinitionLink *link = find_def_link_at(symbols, num_symbols, cursor);
    if (!link) return NULL;

    const char *target_uri = link->target_uri ? link->target_uri : uri;
    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri", target_uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, link->target->selection_range));
    return location;
}
