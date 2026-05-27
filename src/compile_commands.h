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

/**
 * One entry parsed out of compile_commands.json.  Mirrors the standard
 * compile_commands.json schema (directory + file + command), narrowed to
 * the subset taskjuggler-lsp consumes:
 *   - `file_abs`  — absolute path to a top-level .tjp.  Resolved by
 *                   compile_commands_load() from the entry's `file`
 *                   against its `directory` (or against the workspace
 *                   root when `directory` is absent).
 *   - `directory` — verbatim `directory` field, owned copy.  Kept for
 *                   future use (e.g. resolving tj3 -I include paths).
 *   - `command`   — verbatim `command` field, owned copy.  Kept for
 *                   future use (scenario flags, -D defines).
 *
 * All three strings are heap-allocated and released by
 * compile_commands_free().
 */
typedef struct {
    char *file_abs;
    char *directory;
    char *command;
} CompileEntry;

/**
 * Reason returned from compile_commands_load() on failure.  Mapped to
 * user-facing error messages by the caller.
 */
typedef enum {
    CC_OK = 0,
    CC_NOT_FOUND,     /**< compile_commands.json missing or unreadable */
    CC_PARSE_ERROR,   /**< JSON is malformed */
    CC_SCHEMA_ERROR,  /**< JSON parsed but does not match expected schema */
    CC_NO_ROOT,       /**< workspace_root is NULL */
} CompileCommandsResult;

/**
 * Load and parse `<workspace_root>/compile_commands.json`.
 *
 * On success (return value `CC_OK`), `*out_entries` is set to a
 * heap-allocated array of length `*out_count` (zero-length array
 * possible when the file is `[]`; in that case `*out_entries` is NULL).
 * The caller is responsible for releasing the array via
 * compile_commands_free().
 *
 * On failure, `*out_entries` is set to NULL and `*out_count` to 0; the
 * returned enum identifies the failure mode so the caller can choose
 * the right user-facing message.
 *
 * @param workspace_root  Filesystem path to the workspace root.  May be NULL.
 * @param out_entries     Out: array of entries.
 * @param out_count       Out: number of entries.
 * @return CC_OK or a CompileCommandsResult error code.
 */
CompileCommandsResult compile_commands_load(const char *workspace_root,
                                             CompileEntry **out_entries,
                                             int *out_count);

/** Free an array returned by compile_commands_load(). */
void compile_commands_free(CompileEntry *entries, int count);
