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

#include "compile_commands.h"

#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Concatenate @p a, '/' if needed, and @p b into a newly-allocated string.
 * Caller is responsible for freeing the returned pointer.
 *
 * @param a  Left-hand path component (must not be NULL).
 * @param b  Right-hand path component (must not be NULL).
 * @return   Heap-allocated concatenated path, or NULL on allocation failure.
 */
static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != '/');
    char *out = malloc(la + (need_sep ? 1 : 0) + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    size_t off = la;
    if (need_sep) out[off++] = '/';
    memcpy(out + off, b, lb + 1);
    return out;
}

/**
 * Resolve @p file against @p directory (or @p workspace_root when @p directory
 * is NULL or empty) into a newly-allocated absolute path.
 * If @p file is already absolute it is duplicated directly.
 *
 * @param workspace_root  Fallback base directory used when @p directory is
 *                        NULL or empty.
 * @param directory       Preferred base directory from the compile-commands
 *                        entry, or NULL.
 * @param file            Source file path from the compile-commands entry;
 *                        may be absolute or relative.
 * @return                Heap-allocated absolute path, or NULL if @p file is
 *                        NULL or no usable base is available.
 */
static char *resolve_entry_path(const char *workspace_root,
                                 const char *directory,
                                 const char *file) {
    if (!file) return NULL;
    if (file[0] == '/') return strdup(file);
    const char *base = (directory && directory[0]) ? directory : workspace_root;
    if (!base) return NULL;
    return path_join(base, file);
}

void compile_commands_free(CompileEntry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].file_abs);
        free(entries[i].directory);
        free(entries[i].command);
    }
    free(entries);
}

/** Decode one compile_commands.json array element into @p out.
 *
 *  Requires a string "file" field; "directory" and "command" are optional.
 *  The file is resolved to an absolute path against its directory (or the
 *  workspace root). The "directory" and "command" strings are copied.
 *
 *  @param workspace_root  Fallback base for resolving relative file paths.
 *  @param item            One array element from compile_commands.json.
 *  @param out             Entry to populate on success.
 *  @return 1 if @p out was populated, 0 if the element was skipped (not an
 *          object, missing/invalid "file", or an unresolvable path). */
static int parse_compile_entry(const char *workspace_root, yyjson_val *item,
                               CompileEntry *out) {
    if (!yyjson_is_obj(item)) return 0;

    yyjson_val *file_v      = yyjson_obj_get(item, "file");
    yyjson_val *directory_v = yyjson_obj_get(item, "directory");
    yyjson_val *command_v   = yyjson_obj_get(item, "command");

    if (!file_v || !yyjson_is_str(file_v)) return 0;

    const char *file_s = yyjson_get_str(file_v);
    const char *dir_s  = (directory_v && yyjson_is_str(directory_v))
                          ? yyjson_get_str(directory_v) : NULL;
    const char *cmd_s  = (command_v && yyjson_is_str(command_v))
                          ? yyjson_get_str(command_v) : NULL;

    char *abs_path = resolve_entry_path(workspace_root, dir_s, file_s);
    if (!abs_path) return 0;

    out->file_abs  = abs_path;
    out->directory = dir_s ? strdup(dir_s) : NULL;
    out->command   = cmd_s ? strdup(cmd_s) : NULL;
    return 1;
}

CompileCommandsResult compile_commands_load(const char *workspace_root,
                                             CompileEntry **out_entries,
                                             int *out_count) {
    *out_entries = NULL;
    *out_count   = 0;

    if (!workspace_root) return CC_NO_ROOT;

    char *cc_path = path_join(workspace_root, "compile_commands.json");
    if (!cc_path) return CC_NOT_FOUND;

    /* Cheap existence check first so missing-file is distinguishable
     * from parse failure in the error path. */
    struct stat st;
    if (stat(cc_path, &st) != 0) {
        free(cc_path);
        return CC_NOT_FOUND;
    }

    yyjson_read_err err = {0};
    yyjson_doc *doc = yyjson_read_file(cc_path, 0, NULL, &err);
    free(cc_path);
    if (!doc) {
        fprintf(stderr,
                "taskjuggler-lsp: compile_commands.json parse error at "
                "offset %zu: %s\n", err.pos, err.msg ? err.msg : "(no msg)");
        return CC_PARSE_ERROR;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return CC_SCHEMA_ERROR;
    }

    size_t n = yyjson_arr_size(root);
    CompileEntry *entries = n ? calloc(n, sizeof(CompileEntry)) : NULL;
    if (n && !entries) {
        yyjson_doc_free(doc);
        return CC_SCHEMA_ERROR;
    }

    int written = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        if (parse_compile_entry(workspace_root, item, &entries[written]))
            written++;
    }

    yyjson_doc_free(doc);

    *out_entries = entries;
    *out_count   = written;
    return CC_OK;
}
