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

#include "diagnostics.h"
#include "server.h"

#include <yyjson.h>
#include <stdlib.h>

/* TODO(diagnostics): see diagnostics.h.  This stub publishes an empty
 * diagnostics array so the editor clears stale markers whenever the
 * server would previously have re-published.  Once diagnostic
 * collection is restored, this function will serialise the rebuilt
 * diagnostic set. */
void publish_diagnostics(const char *uri) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *diag_arr = yyjson_mut_arr(doc);

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
    if (text) {
        lsp_send_message(text);
        free(text);
    }
}
