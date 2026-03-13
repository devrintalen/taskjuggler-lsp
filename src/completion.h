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

#pragma once

#include "parser.h"
#include "grammar.tab.h"
#include <cjson/cJSON.h>

/*
 * Return a JSON CompletionList object (or cJSON_CreateNull() if no items)
 * for the given cursor position.  Caller owns the returned cJSON object.
 */
cJSON *build_completions_json(const TokenSpan *tokens, int num_tokens, LspPos cursor,
                              const DocSymbol *symbols, int num_symbols);
