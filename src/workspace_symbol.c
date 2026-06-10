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

#include "workspace_symbol.h"
#include "document_symbol.h"  /* symbol_kind_for() */
#include "rpc.h"

#include <string.h>
#include <strings.h>

/** Append one WorkspaceSymbol (SymbolInformation) entry for @p sym to @p arr:
 *  its name, kind, optional containerName, and a Location in @p uri.
 *  @param doc        Destination mutable JSON document.
 *  @param sym        Symbol to serialize.
 *  @param name       Display name (already defaulted from sym->name).
 *  @param uri        Document URI the symbol lives in.
 *  @param container  Enclosing symbol name, or NULL for top-level.
 *  @param arr        SymbolInformation[] array to append to. */
static void emit_workspace_symbol(yyjson_mut_doc *doc, const tj_node *sym,
                                  const char *name, const char *uri,
                                  const char *container, yyjson_mut_val *arr) {
    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, entry, "name", name);
    yyjson_mut_obj_add_uint(doc, entry, "kind", (uint64_t)symbol_kind_for(sym->keyword));
    if (container)
        yyjson_mut_obj_add_str(doc, entry, "containerName", container);

    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri", uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, sym->selection_range));
    yyjson_mut_obj_add_val(doc, entry, "location", location);

    yyjson_mut_arr_add_val(arr, entry);
}

/**
 * Recursively walk @p syms, appending matching SymbolInformation entries
 * to @p arr.  Recurses into every node's children regardless of whether
 * the node itself matched.
 *
 * @param doc        Destination mutable JSON document.
 * @param query      Case-insensitive substring filter; `""` matches
 *                   everything.
 * @param syms       Sibling symbols to search.
 * @param n          Length of @p syms.
 * @param uri        Document URI to embed in each Location result.
 * @param container  Name of the parent symbol, or NULL at the top level.
 * @param arr        SymbolInformation[] array to append matches to.
 */
static void collect_recursive(yyjson_mut_doc *doc, const char *query,
                               tj_node *const *syms, int n,
                               const char *uri, const char *container,
                               yyjson_mut_val *arr)
{
    for (int i = 0; i < n; i++) {
        const tj_node *sym = syms[i];
        const char *name = sym->name ? sym->name : "";

        /* Empty query matches everything; otherwise case-insensitive substring. */
        int matches = (query[0] == '\0') || (strcasestr(name, query) != NULL);

        if (matches)
            emit_workspace_symbol(doc, sym, name, uri, container, arr);

        if (sym->num_children > 0)
            collect_recursive(doc, query,
                              sym->children, sym->num_children,
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
                                tj_node *const *syms, int n,
                                const char *uri, yyjson_mut_val *arr)
{
    collect_recursive(doc, query, syms, n, uri, NULL, arr);
}

yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                        yyjson_val *params,
                                        const query_context *qc) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < qc->num_docs; i++) {
        if (!qc->docs[i].root) continue;
        tj_node *const *top; int n;
        doc_symbol_pool(&qc->docs[i], &top, &n);
        if (!top) continue;
        collect_workspace_symbols(doc, query, top, n, qc->docs[i].uri, arr);
    }
    return make_response(doc, id, arr);
}
