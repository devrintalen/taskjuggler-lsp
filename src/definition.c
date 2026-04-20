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
 * At query time, build_definition_json() locates the innermost DocSymbol
 * containing the cursor via symbol_at() and scans its def_links for one
 * whose source range covers the cursor.  When found, returns a Location
 * object pointing at the target symbol's selection_range.
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

/* Find a DefinitionLink whose source range contains the cursor.  Uses the
 * precomputed tok_spans[].owner to locate the innermost enclosing DocSymbol
 * via symbol_at(), then walks up the parent chain checking def_links at
 * each level.  Returns a pointer to the matching link, or NULL. */
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

/* Build a Location JSON object for the go-to-definition response.
 * Locates a DefinitionLink whose source range contains cursor.
 * Returns NULL if no matching link is found (server will return null to editor).
 *
 * doc        — the mutable JSON document that will own the returned value
 * tokens     — token span array from the ParseResult (carries .owner links)
 * num_tokens — number of entries in tokens
 * cursor     — cursor position from the textDocument/definition request
 * uri        — URI of the requesting document, used as fallback target URI
 */
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
