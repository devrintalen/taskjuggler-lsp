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

#pragma once

#include "parser.h"
#include "grammar.tab.h"
#include "query_context.h"   /* query_doc */
#include <yyjson.h>

/** Result of scanning context up to a cursor position. */
typedef struct {
    char    *keyword;    /**< heap-allocated; NULL if no active context */
    uint32_t arg_count;  /**< number of completed argument tokens at cursor */
} ActiveContext;

/**
 * Scan tokens up to @p cursor and return the innermost active statement
 * keyword together with the number of completed argument tokens.
 * Caller must free result.keyword (if non-NULL).
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return Active keyword and argument count; `{NULL, 0}` when no
 *         keyword's argument list encompasses @p cursor.
 */
ActiveContext active_context(const TokenSpan *tokens, int num_tokens, LspPos cursor);

/**
 * Build an LSP SignatureHelp JSON object for @p kw with @p active_param
 * highlighted, or NULL if @p kw has no known signature.
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc           Destination mutable JSON document.
 * @param kw            Keyword text (e.g. `"task"`, `"depends"`).
 * @param active_param  Zero-indexed argument the cursor is on.
 * @return The SignatureHelp JSON object, or NULL when @p kw has no
 *         registered signature.
 */
yyjson_mut_val *build_signature_help_json(yyjson_mut_doc *doc,
                                           const char *kw,
                                           uint32_t active_param);

/**
 * Handle "textDocument/signatureHelp": return parameter signature
 * information for the keyword active at the cursor position.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params containing a "position" object.
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response with a SignatureHelp object, or null.
 */
yyjson_mut_val *handle_signature_help(yyjson_mut_doc *doc, yyjson_val *id,
                                      yyjson_val *params, const query_doc *d);
