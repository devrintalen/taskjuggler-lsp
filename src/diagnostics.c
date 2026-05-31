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
 * diagnostic set.
 *
 * TODO(diagnostics): "Missing compile_commands.json" warnings.  When the
 * server has no usable compile_commands.json (no workspace root, or the
 * file is absent), it loads no project closures and every editor file is
 * parsed stand-alone.  We replaced the old window/showMessage with
 * per-file warning diagnostics; emit them here, gated on a global
 * cc-status flag set in reload_compile_commands() (see the two
 * TODO(diagnostics) markers there — e.g. g_cc_missing).  Two cases:
 *
 *   1. .tjp files — one Warning per `include` directive, located at the
 *      include statement.  The IncludeRef carries no range, but each
 *      directive is a KW_INCLUDE token in the document's ParseOutput
 *      tok_spans[]; scan for kind == KW_INCLUDE and use that span's
 *      start/end.  Message similar to:
 *        "Missing compile_commands.json, cross-file LSP features are
 *         disabled."
 *
 *   2. .tji files — one Warning at the top of the file (range covering
 *      line 0), since an include fragment opened with no including .tjp
 *      is parsed in isolation.  Message similar to:
 *        "Missing compile_commands.json, this file will be parsed
 *         stand-alone, not as part of any other loaded .tjp file that
 *         includes it."
 *
 * Both use DIAG_WARNING severity.  Reaching the ParseOutput/tok_spans for
 * @p uri from here needs a small accessor over the server's docs[] (or
 * move this builder into server.c, where the document store lives). */
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
