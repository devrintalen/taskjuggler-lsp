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

#include "query_context.h"   /* query_context, query_doc */
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

/**
 * Build an LSP SemanticTokensDelta object describing the minimal patch
 * that transforms @p prev_buf into @p new_buf.
 *
 * @code{.json}
 * { "resultId": "...", "edits": [ { "start": 0, "deleteCount": 5, "data": [...] } ] }
 * @endcode
 *
 * Both buffers are flat uint32 arrays in the LSP five-integer-per-token
 * delta encoding (multiple of 5).  The diff treats each five-tuple as a
 * single element, so all edits in the result are aligned to 5-int
 * boundaries.
 *
 * Computation: a longest-common-prefix and longest-common-suffix trim is
 * applied first (handling the typical edit-one-line case in O(N)
 * without an allocator), then Myers' O(ND) diff runs on the remaining
 * middle slices.  Adjacent insert/delete operations are coalesced into a
 * single SemanticTokensEdit.
 *
 * @param doc        Destination mutable JSON document.
 * @param prev_buf   Previously-returned data buffer; may be NULL only
 *                   when @p prev_count is 0.
 * @param prev_count Number of uint32 entries in @p prev_buf.
 * @param new_buf    Newly computed data buffer; may be NULL only when
 *                   @p new_count is 0.
 * @param new_count  Number of uint32 entries in @p new_buf.
 * @param result_id  Result id string to include alongside the edits.
 *                   May be NULL to omit the field.
 * @return The SemanticTokensDelta JSON object.
 */
yyjson_mut_val *build_semantic_tokens_delta_json(yyjson_mut_doc *doc,
                                                  const uint32_t *prev_buf, size_t prev_count,
                                                  const uint32_t *new_buf,  size_t new_count,
                                                  const char *result_id);

/**
 * Handle "textDocument/semanticTokens/full/delta": diff the current token
 * buffer against the snapshot the client last received (identified by
 * @c previousResultId), falling back to a full response when no matching
 * base snapshot is available.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params with an optional "previousResultId" string.
 * @param qc      Query context; consulted for the previous snapshot.
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response with a SemanticTokensDelta or full
 *         SemanticTokens object.
 */
yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                  yyjson_val *params,
                                                  const query_context *qc,
                                                  const query_doc *d);
