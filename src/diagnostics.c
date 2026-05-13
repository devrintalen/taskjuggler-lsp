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

/* See doc/modules/diagnostics.rst for the module overview. */

#include "diagnostics.h"
#include "parser.h"
#include "server.h"

#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Diagnostic accumulation ─────────────────────────────────────────────── */

/* Append a diagnostic to r's diagnostics array, growing it if needed.
 * range    — source range to highlight in the editor
 * severity — DIAG_ERROR or DIAG_WARNING
 * msg      — human-readable message; a heap copy is made and owned by r
 */
void push_diagnostic(ParseResult *r, LspRange range, int severity,
                     const char *msg) {
    if (r->num_diagnostics >= r->diag_cap) {
        int nc = r->diag_cap ? r->diag_cap * 2 : 4;
        Diagnostic *tmp = realloc(r->diagnostics,
                                  (size_t)nc * sizeof(Diagnostic));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->diagnostics = tmp;
        r->diag_cap = nc;
    }
    r->diagnostics[r->num_diagnostics++] =
        (Diagnostic){ range, severity, strdup(msg) };
}

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

void publish_diagnostics(const char *uri, const ParseResult *r) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *diag_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < r->num_diagnostics; i++) {
        const Diagnostic *d = &r->diagnostics[i];
        yyjson_mut_val *dj = yyjson_mut_obj(doc);

        yyjson_mut_val *range = yyjson_mut_obj(doc);
        yyjson_mut_val *start = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, start, "line",      d->range.start.line);
        yyjson_mut_obj_add_uint(doc, start, "character", d->range.start.character);
        yyjson_mut_val *end = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, end, "line",      d->range.end.line);
        yyjson_mut_obj_add_uint(doc, end, "character", d->range.end.character);
        yyjson_mut_obj_add_val(doc, range, "start", start);
        yyjson_mut_obj_add_val(doc, range, "end",   end);
        yyjson_mut_obj_add_val(doc,  dj, "range",    range);
        yyjson_mut_obj_add_uint(doc, dj, "severity", (uint64_t)d->severity);
        yyjson_mut_obj_add_str(doc,  dj, "message",  d->message);
        yyjson_mut_arr_add_val(diag_arr, dj);
    }

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, params, "uri", uri);
    yyjson_mut_obj_add_val(doc, params, "diagnostics", diag_arr);

    yyjson_mut_val *notif = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, notif, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, notif, "method",  "textDocument/publishDiagnostics");
    yyjson_mut_obj_add_val(doc, notif, "params",  params);

    yyjson_mut_doc_set_root(doc, notif);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    lsp_send_message(text);
    free(text);
}
