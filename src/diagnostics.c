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

#include "diagnostics.h"
#include "server.h"

#include <yyjson.h>
#include <stdlib.h>
#include <string.h>

/* ── publishDiagnostics notification ─────────────────────────────────────── */

void publish_diagnostics_list(const char *uri, const Diagnostic *diags, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *diag_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        const Diagnostic *d = &diags[i];

        yyjson_mut_val *start = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, start, "line",      d->range.start.line);
        yyjson_mut_obj_add_uint(doc, start, "character", d->range.start.character);

        yyjson_mut_val *end = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, end, "line",      d->range.end.line);
        yyjson_mut_obj_add_uint(doc, end, "character", d->range.end.character);

        yyjson_mut_val *range = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, range, "start", start);
        yyjson_mut_obj_add_val(doc, range, "end",   end);

        yyjson_mut_val *diag = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, diag, "range",    range);
        yyjson_mut_obj_add_int(doc, diag, "severity", d->severity);
        if (d->source)
            yyjson_mut_obj_add_str(doc, diag, "source", d->source);
        yyjson_mut_obj_add_str(doc, diag, "message", d->message ? d->message : "");

        yyjson_mut_arr_append(diag_arr, diag);
    }

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, params, "uri", uri);
    yyjson_mut_obj_add_val(doc, params, "diagnostics", diag_arr);

    yyjson_mut_val *notif = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, notif, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, notif, "method",  "textDocument/publishDiagnostics");
    yyjson_mut_obj_add_val(doc, notif, "params",  params);

    yyjson_mut_doc_set_root(doc, notif);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (text) {
        lsp_send_message(text);
        free(text);
    }
}

void publish_diagnostics(const char *uri) {
    publish_diagnostics_list(uri, NULL, 0);
}

/* TODO(diagnostics): "Missing compile_commands.json" warnings.  This is a
 * separate, server-level diagnostic source, independent of the tj3 diagnostics
 * and still unimplemented.  When the server has no usable compile_commands.json
 * (no workspace root, or the file is absent), it loads no project closures and
 * every editor file is parsed stand-alone.  We replaced the old
 * window/showMessage with per-file warning diagnostics; produce them into a
 * diag_set (so they merge with any other source for the same URI) and publish
 * via diag_set_publish, gated on a global cc-status flag set in
 * reload_compile_commands() (see the two TODO(diagnostics) markers there —
 * e.g. g_cc_missing).  Two cases:
 *
 *   1. .tjp files — one Warning per `include` directive, located at the
 *      include statement.  The IncludeRef carries no range, but each directive
 *      is a KW_INCLUDE token in the document's token spans; scan for
 *      kind == KW_INCLUDE and use that span's start/end.  Message similar to:
 *        "Missing compile_commands.json, cross-file LSP features are
 *         disabled."
 *
 *   2. .tji files — one Warning at the top of the file (range covering
 *      line 0), since an include fragment opened with no including .tjp is
 *      parsed in isolation.  Message similar to:
 *        "Missing compile_commands.json, this file will be parsed
 *         stand-alone, not as part of any other loaded .tjp file that
 *         includes it."
 *
 * Both use DIAG_WARNING severity.  Reaching the token spans for the URI needs
 * a small accessor over the server's docs[] (or building this source in
 * server.c, where the document store lives). */

/* ── diag_set ────────────────────────────────────────────────────────────── */

typedef struct diag_file {
    char       *uri;     /* owned */
    Diagnostic *items;   /* owned; each .message owned */
    int         count;
    int         cap;
} diag_file;

struct diag_set {
    diag_file *files;    /* owned */
    int        count;
    int        cap;
};

diag_set *diag_set_new(void) {
    diag_set *s = calloc(1, sizeof(*s));
    if (!s) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    return s;
}

static diag_file *diag_set_file(diag_set *s, const char *uri) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->files[i].uri, uri) == 0)
            return &s->files[i];

    if (s->count >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        diag_file *tmp = realloc(s->files, (size_t)nc * sizeof(diag_file));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        s->files = tmp;
        s->cap   = nc;
    }
    diag_file *f = &s->files[s->count++];
    f->uri   = strdup(uri);
    f->items = NULL;
    f->count = 0;
    f->cap   = 0;
    return f;
}

void diag_set_add(diag_set *s, const char *uri, Diagnostic d) {
    diag_file *f = diag_set_file(s, uri);
    if (f->count >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 4;
        Diagnostic *tmp = realloc(f->items, (size_t)nc * sizeof(Diagnostic));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        f->items = tmp;
        f->cap   = nc;
    }
    f->items[f->count++] = d;
}

void diag_set_free(diag_set *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) {
        for (int j = 0; j < s->files[i].count; j++)
            free(s->files[i].items[j].message);
        free(s->files[i].items);
        free(s->files[i].uri);
    }
    free(s->files);
    free(s);
}

static int diag_set_has(const diag_set *s, const char *uri) {
    if (!s) return 0;
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->files[i].uri, uri) == 0)
            return 1;
    return 0;
}

void diag_set_publish(const diag_set *current, const diag_set *previous) {
    if (current) {
        for (int i = 0; i < current->count; i++)
            publish_diagnostics_list(current->files[i].uri,
                                     current->files[i].items,
                                     current->files[i].count);
    }
    if (previous) {
        for (int i = 0; i < previous->count; i++)
            if (!diag_set_has(current, previous->files[i].uri))
                publish_diagnostics(previous->files[i].uri);
    }
}
