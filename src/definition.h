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

#include "project_tree.h"
#include "query_context.h"   /* query_context, query_doc */
#include <yyjson.h>

/**
 * Build the textDocument/definition response for a resolved dependency target.
 *
 * Returns a single LSP `Location` pointing at @p target's identifier, or NULL
 * — serialised as JSON `null` by the caller — when @p target is NULL (the
 * reference did not resolve). Callers obtain @p target via
 * project_resolve_dep_ref().
 *
 * @param doc     Destination mutable JSON document.
 * @param target  The resolved target task ProjectNode, or NULL.
 * @return A `Location` JSON object, or NULL.
 */
yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc, ProjectNode *target);

/**
 * Handle "textDocument/definition": resolve the dependency reference under
 * the cursor to the target task's declaration location.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params containing a "position" object.
 * @param qc      Query context with the assembled project tree.
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response with a Location or null result.
 */
yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params, const query_context *qc,
                                  const query_doc *d);
