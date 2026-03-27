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

#include "workspace_symbol.h"
#include "document_symbol.h"

#include <string.h>
#include <strings.h>

/* Recursively walk syms[], appending matching SymbolInformation entries to arr.
 * Recurses into every node's children regardless of whether the node matched.
 *
 * doc       — the mutable JSON document that will own new values
 * query     — case-insensitive substring filter; "" matches everything
 * syms      — array of symbols to search
 * n         — number of entries in syms
 * uri       — document URI to embed in each Location result
 * container — name of the parent symbol, or NULL at the top level
 * arr       — JSON array to append matching entries to
 */
static void collect_recursive(yyjson_mut_doc *doc, const char *query,
                               const DocSymbol *syms, int n,
                               const char *uri, const char *container,
                               yyjson_mut_val *arr)
{
    for (int i = 0; i < n; i++) {
        const DocSymbol *sym = &syms[i];
        const char *name = sym->name ? sym->name : "";

        /* Empty query matches everything; otherwise case-insensitive substring. */
        int matches = (query[0] == '\0') || (strcasestr(name, query) != NULL);

        if (matches) {
            yyjson_mut_val *entry = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, entry, "name", name);
            yyjson_mut_obj_add_uint(doc, entry, "kind", (uint64_t)sym->kind);
            if (container)
                yyjson_mut_obj_add_str(doc, entry, "containerName", container);

            yyjson_mut_val *location = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, location, "uri", uri);
            yyjson_mut_obj_add_val(doc, location, "range",
                                   range_json(doc, sym->selection_range));
            yyjson_mut_obj_add_val(doc, entry, "location", location);

            yyjson_mut_arr_add_val(arr, entry);
        }

        if (sym->num_children > 0)
            collect_recursive(doc, query, sym->children, sym->num_children,
                              uri, name, arr);
    }
}

/* Append all symbols from syms[] that match query to the JSON array arr.
 * Entry point called by the server for each open document.
 *
 * doc   — the mutable JSON document that will own new values
 * query — case-insensitive substring filter; "" matches everything
 * syms  — root-level symbol array for this document
 * n     — number of entries in syms
 * uri   — document URI used in Location results
 * arr   — shared JSON array to append results to (across all documents)
 */
void collect_workspace_symbols(yyjson_mut_doc *doc, const char *query,
                                const DocSymbol *syms, int n,
                                const char *uri, yyjson_mut_val *arr)
{
    collect_recursive(doc, query, syms, n, uri, NULL, arr);
}
