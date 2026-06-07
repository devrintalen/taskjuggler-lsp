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
#include "project_tree.h"
#include "query_context.h"   /* query_context, query_doc */
#include <yyjson.h>

/**
 * Build a DocumentHighlight[] JSON array for textDocument/documentHighlight.
 *
 * Works bidirectionally: the caller resolves the cursor — whether it sits on
 * a task declaration or on a dependency reference — to a single target task
 * @p wanted in the pinned snapshot's ProjectNode tree (the same resolution
 * definition/references use).  This builder then reports, scoped to the
 * current document only (highlight ranges carry no URI):
 *   - @p wanted's declaration identifier as Write (kind 3), when the
 *     declaration lives in this document, and
 *   - the matching identifier token of every dependency that resolves to
 *     @p wanted as Read (kind 2).
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc           Destination mutable JSON document.
 * @param project_root  The requesting project's synthetic root, the
 *                      resolution context forwarded to project_dep_resolve().
 * @param wanted        The resolved target task, or NULL.
 * @param doc_uri       URI of the current document; scopes highlights to it.
 * @param tokens        Token spans of the current document, used to pick the
 *                      target segment's identifier token out of a dependency's
 *                      full source range.
 * @param num_tokens    Length of @p tokens.
 * @return A JSON array of DocumentHighlight objects, or NULL when @p wanted
 *         (or a required input) is NULL.
 */
yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    ProjectNode *project_root,
    const ProjectNode *wanted,
    const char *doc_uri,
    const TokenSpan *tokens, int num_tokens);

/**
 * Handle "textDocument/documentHighlight": highlight all occurrences of the
 * task under the cursor (declaration and references) within the primary
 * document.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params containing a "position" object.
 * @param qc      Query context with the assembled project tree.
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response containing the highlight array.
 */
yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params,
                                          const query_context *qc,
                                          const query_doc *d);
