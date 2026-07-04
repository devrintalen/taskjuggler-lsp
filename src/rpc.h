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

/** @file
 *  JSON-RPC envelope construction and request-parameter extraction helpers
 *  shared by the coordinator (server.c) and every feature query handler.
 *  These are pure JSON utilities with no dependency on the document store. */

#pragma once

#include "parser.h"   /* LspPos */
#include <yyjson.h>

/**
 * Extract an LSP Position object from the JSON value @p obj.
 *
 * @param obj  A yyjson object with "line" and "character" numeric fields.
 * @return The decoded position; both fields default to 0 on missing keys.
 */
LspPos json_to_pos(yyjson_val *obj);

/**
 * Return the string value of @p key inside the JSON object @p obj, or NULL
 * when @p obj is NULL, the key is absent, or the value is not a string.
 *
 * @param obj  JSON object to query.
 * @param key  Key name to look up.
 * @return Borrowed string pointer into the parsed document, or NULL.
 */
const char *json_str(yyjson_val *obj, const char *key);

/**
 * Build a JSON-RPC 2.0 success response object containing @p result.
 *
 * @param doc     Mutable document that will own the response object.
 * @param id      Request id to echo back; copied preserving its JSON type.
 * @param result  Value to place in the "result" field.
 * @return A mutable JSON object representing the response.
 */
yyjson_mut_val *make_response(yyjson_mut_doc *doc, yyjson_val *id,
                              yyjson_mut_val *result);

/**
 * Build a JSON-RPC 2.0 error response object.
 *
 * @param doc      Mutable document that will own the response object.
 * @param id       Request id to echo back; copied preserving its JSON type.
 * @param code     JSON-RPC error code (e.g. -32800 for request-cancelled).
 * @param message  Human-readable error message.
 * @return A mutable JSON object representing the error response.
 */
yyjson_mut_val *make_error_response(yyjson_mut_doc *doc, yyjson_val *id,
                                    int code, const char *message);

/**
 * Write one LSP-framed message to stdout, prepending the required
 * `Content-Length` header.  Safe to call from any thread; serialized
 * internally by a mutex.
 *
 * @param msg  NUL-terminated JSON message body.
 */
void lsp_send_message(const char *msg);

/**
 * Send a window/showMessage notification to the client.  Used to
 * surface non-fatal load/configuration errors (e.g. a removed
 * compile_commands.json) without crashing the session.
 *
 * @param type     LSP MessageType: 1=Error, 2=Warning, 3=Info, 4=Log.
 * @param message  Human-readable message text to display.
 */
void show_message(int type, const char *message);
