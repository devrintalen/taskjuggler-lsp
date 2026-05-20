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
#include <yyjson.h>

/*
 * TODO(definition): textDocument/definition was driven by per-symbol
 * DefinitionLinks computed during dep-ref resolution.  Both have been
 * removed for the tj_node refactor.  build_definition_json() is
 * preserved as a stub that always reports "no definition" so the
 * dispatch path keeps compiling; restore real lookup once the global
 * task tree and dependency resolution are reinstated.
 */

/**
 * Stub: always returns NULL so the LSP response becomes JSON null.
 *
 * @param doc         Destination mutable JSON document (unused).
 * @param tokens      Token spans of the current document (unused).
 * @param num_tokens  Length of @p tokens (unused).
 * @param cursor      Cursor position (unused).
 * @param uri         URI placed into the response (unused).
 * @return Always NULL.
 */
yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor, const char *uri);
