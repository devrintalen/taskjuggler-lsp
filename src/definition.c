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
 * Go-to-definition is answered from the DefinitionLink array stored in
 * ParseResult.  Each DefinitionLink records a pair of ranges:
 *
 *   source — the range of a reference expression in the document (e.g. a
 *             dependency path in a `depends` or `precedes` clause)
 *   target — the selection_range of the symbol being referred to
 *
 * These links are populated by diagnostics.c:revalidate_dep_refs() after
 * every document change for every successfully resolved dependency reference.
 *
 * At query time, build_definition_json() scans the link array for an entry
 * whose source range contains the cursor and, when found, returns a Location
 * object pointing at the target symbol's selection_range in the same document.
 *
 * ── Supported references ─────────────────────────────────────────────────
 *
 * Currently only task dependency references (`depends` / `precedes` paths)
 * produce definition links.  Resource references (`allocate`, `responsible`,
 * `booking`) are not yet tracked.
 */

#include "definition.h"
#include "document_symbol.h"

static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       const DefinitionLink *links, int num_links,
                                       LspPos cursor, const char *uri) {
    for (int i = 0; i < num_links; i++) {
        if (pos_in_range(cursor, links[i].source)) {
            const char *target_uri = links[i].target_uri ? links[i].target_uri : uri;
            yyjson_mut_val *location = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, location, "uri", target_uri);
            yyjson_mut_obj_add_val(doc, location, "range",
                                   range_json(doc, links[i].target));
            return location;
        }
    }
    return NULL;
}
