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

#include "document_symbol.h"

cJSON *range_json(LspRange r) {
    cJSON *s = cJSON_CreateObject();
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "line",      r.start.line);
    cJSON_AddNumberToObject(st, "character", r.start.character);
    cJSON *en = cJSON_CreateObject();
    cJSON_AddNumberToObject(en, "line",      r.end.line);
    cJSON_AddNumberToObject(en, "character", r.end.character);
    cJSON_AddItemToObject(s, "start", st);
    cJSON_AddItemToObject(s, "end",   en);
    return s;
}

static cJSON *doc_symbol_to_json(const DocSymbol *sym) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name",   sym->name   ? sym->name   : "");
    cJSON_AddStringToObject(obj, "detail", sym->detail ? sym->detail : "");
    cJSON_AddNumberToObject(obj, "kind",   sym->kind);
    cJSON_AddItemToObject(obj, "range",          range_json(sym->range));
    cJSON_AddItemToObject(obj, "selectionRange", range_json(sym->selection_range));
    if (sym->num_children > 0) {
        cJSON *ch = cJSON_CreateArray();
        for (int i = 0; i < sym->num_children; i++)
            cJSON_AddItemToArray(ch, doc_symbol_to_json(&sym->children[i]));
        cJSON_AddItemToObject(obj, "children", ch);
    }
    return obj;
}

cJSON *build_document_symbols_json(const DocSymbol *syms, int n) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, doc_symbol_to_json(&syms[i]));
    return arr;
}
