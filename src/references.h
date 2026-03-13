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
 * Build a Location[] JSON array for textDocument/references.
 *
 * Returns null when the cursor is not on a task declaration identifier.
 * Returns an empty array when the cursor is on a task declaration that
 * no dependency reference points to.
 * Otherwise returns an array of Location objects, one per dependency
 * reference that resolves to the task under the cursor.
 *
 * Only task declarations (SK_FUNCTION) are recognised as trigger points.
 * Only dependency references recorded as DefinitionLinks are returned;
 * resource references (allocate, responsible, booking) are not included.
 *
 * The caller is responsible for cJSON_Delete()ing the returned object.
 */
cJSON *build_references_json(const DefinitionLink *links, int num_links,
                             const DocSymbol *symbols, int num_symbols,
                             LspPos cursor, const char *uri);
