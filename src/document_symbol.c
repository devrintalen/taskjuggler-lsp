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

yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r) {
    yyjson_mut_val *s  = yyjson_mut_obj(doc);
    yyjson_mut_val *st = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, st, "line",      r.start.line);
    yyjson_mut_obj_add_uint(doc, st, "character", r.start.character);
    yyjson_mut_val *en = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, en, "line",      r.end.line);
    yyjson_mut_obj_add_uint(doc, en, "character", r.end.character);
    yyjson_mut_obj_add_val(doc, s, "start", st);
    yyjson_mut_obj_add_val(doc, s, "end",   en);
    return s;
}

static yyjson_mut_val *doc_symbol_to_json(yyjson_mut_doc *doc,
                                           const DocSymbol *sym) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "name",   sym->name   ? sym->name   : "");
    yyjson_mut_obj_add_str(doc, obj, "detail", sym->detail ? sym->detail : "");
    yyjson_mut_obj_add_uint(doc, obj, "kind",  (uint64_t)sym->kind);
    yyjson_mut_obj_add_val(doc, obj, "range",          range_json(doc, sym->range));
    yyjson_mut_obj_add_val(doc, obj, "selectionRange", range_json(doc, sym->selection_range));
    if (sym->num_children > 0) {
        yyjson_mut_val *ch = yyjson_mut_arr(doc);
        for (int i = 0; i < sym->num_children; i++)
            yyjson_mut_arr_add_val(ch, doc_symbol_to_json(doc, &sym->children[i]));
        yyjson_mut_obj_add_val(doc, obj, "children", ch);
    }
    return obj;
}

yyjson_mut_val *build_document_symbols_json(yyjson_mut_doc *doc,
                                             const DocSymbol *syms, int n) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < n; i++)
        yyjson_mut_arr_add_val(arr, doc_symbol_to_json(doc, &syms[i]));
    return arr;
}
