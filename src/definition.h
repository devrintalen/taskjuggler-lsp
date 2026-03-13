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
#include <cjson/cJSON.h>

/*
 * Build a Location JSON object for textDocument/definition.
 *
 * Searches def_links for a link whose source range contains cursor.  When
 * found, returns a JSON object of the form:
 *
 *   { "uri": "<uri>", "range": <selection_range of target symbol> }
 *
 * Returns NULL when no definition link covers the cursor position.  The
 * caller is responsible for cJSON_Delete()ing the returned object.
 */
cJSON *build_definition_json(const DefinitionLink *links, int num_links,
                             LspPos cursor, const char *uri);
