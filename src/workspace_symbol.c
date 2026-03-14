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

static void collect_recursive(const char *query,
                               const DocSymbol *syms, int n,
                               const char *uri, const char *container,
                               cJSON *arr)
{
    for (int i = 0; i < n; i++) {
        const DocSymbol *sym = &syms[i];
        const char *name = sym->name ? sym->name : "";

        /* Empty query matches everything; otherwise case-insensitive substring. */
        int matches = (query[0] == '\0') || (strcasestr(name, query) != NULL);

        if (matches) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "name", name);
            cJSON_AddNumberToObject(entry, "kind", sym->kind);
            if (container)
                cJSON_AddStringToObject(entry, "containerName", container);

            cJSON *location = cJSON_CreateObject();
            cJSON_AddStringToObject(location, "uri", uri);
            cJSON_AddItemToObject(location, "range", range_json(sym->selection_range));
            cJSON_AddItemToObject(entry, "location", location);

            cJSON_AddItemToArray(arr, entry);
        }

        if (sym->num_children > 0)
            collect_recursive(query, sym->children, sym->num_children,
                              uri, name, arr);
    }
}

void collect_workspace_symbols(const char *query,
                                const DocSymbol *syms, int n,
                                const char *uri, cJSON *arr)
{
    collect_recursive(query, syms, n, uri, NULL, arr);
}
