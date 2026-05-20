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

#include "references.h"

/* TODO(references): textDocument/references previously walked the
 * target task's `ref_links` populated during dep-ref resolution.  Those
 * links were removed for the tj_node refactor.  Stubbed to NULL until
 * dependency resolution returns. */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor) {
    (void)doc; (void)cursor_uri; (void)tokens; (void)num_tokens; (void)cursor;
    return NULL;
}
