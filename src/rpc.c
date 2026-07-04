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

#include "rpc.h"
#include "debug.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LspPos json_to_pos(yyjson_val *obj) {
    LspPos p = {0};
    if (!obj) return p;
    yyjson_val *ln = yyjson_obj_get(obj, "line");
    yyjson_val *ch = yyjson_obj_get(obj, "character");
    if (ln && yyjson_is_num(ln)) p.line      = (uint32_t)yyjson_get_num(ln);
    if (ch && yyjson_is_num(ch)) p.character = (uint32_t)yyjson_get_num(ch);
    return p;
}

const char *json_str(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *item = yyjson_obj_get(obj, key);
    return (item && yyjson_is_str(item)) ? yyjson_get_str(item) : NULL;
}

/** Copy the JSON-RPC request @p id into the mutable document @p doc,
 *  preserving its original type (string, integer, real, or null).
 *  @param doc  Mutable document that will own the new value.
 *  @param id   Immutable id value from the incoming request; may be NULL.
 *  @return     A mutable copy of @p id, or a JSON null when @p id is
 *              NULL or null. */
static yyjson_mut_val *copy_id(yyjson_mut_doc *doc, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(doc);
    if (yyjson_is_str(id))  return yyjson_mut_strcpy(doc, yyjson_get_str(id));
    if (yyjson_is_uint(id)) return yyjson_mut_uint(doc, yyjson_get_uint(id));
    if (yyjson_is_sint(id)) return yyjson_mut_int(doc, yyjson_get_int(id));
    if (yyjson_is_real(id)) return yyjson_mut_real(doc, yyjson_get_real(id));
    return yyjson_mut_null(doc);
}

yyjson_mut_val *make_response(yyjson_mut_doc *doc, yyjson_val *id,
                              yyjson_mut_val *result) {
    yyjson_mut_val *resp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, resp, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, resp, "id", copy_id(doc, id));
    yyjson_mut_obj_add_val(doc, resp, "result", result);
    return resp;
}

yyjson_mut_val *make_error_response(yyjson_mut_doc *doc, yyjson_val *id,
                                    int code, const char *message) {
    yyjson_mut_val *resp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, resp, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, resp, "id", copy_id(doc, id));
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_str(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, resp, "error", err);
    return resp;
}

/* ── Outbound wire I/O ───────────────────────────────────────────────────── */

/** Guards stdout so concurrent worker threads cannot interleave LSP messages. */
static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

void lsp_send_message(const char *msg) {
    DLOG(DEBUG_RPC, LOG_TRACE, "-> %zu byte message", strlen(msg));
    pthread_mutex_lock(&stdout_mutex);
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

void show_message(int type, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, params, "type", type);
    yyjson_mut_obj_add_str(doc, params, "message", message);
    yyjson_mut_val *note = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, note, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, note, "method",  "window/showMessage");
    yyjson_mut_obj_add_val(doc, note, "params",  params);
    yyjson_mut_doc_set_root(doc, note);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (text) {
        lsp_send_message(text);
        free(text);
    }
}
