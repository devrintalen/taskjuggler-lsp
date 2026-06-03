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
#include "query_context.h"
#include "grammar.tab.h"   /* KW_INCLUDE */

#include <yyjson.h>
#include <limits.h>
#include <stdio.h>
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

/* ── "Missing/Malformed compile_commands.json" warnings ──────────────────── */

/* Add one DIAG_WARNING at @p range with a heap copy of @p message to @p out
 * under @p uri. */
static void add_warning(diag_set *out, const char *uri,
                        LspRange range, const char *message) {
    Diagnostic d;
    d.range    = range;
    d.severity = DIAG_WARNING;
    d.source   = "taskjuggler-lsp";
    d.message  = strdup(message);
    diag_set_add(out, uri, d);
}

void diag_collect_cc_missing(const workspace_snapshot *ws,
                             const ws_project *proj, diag_set *out) {
    if (!ws || !proj || !out || ws->cc_status == CC_STATUS_OK) return;

    int pindex = -1;
    for (int i = 0; i < ws->num_projects; i++)
        if (ws->projects[i] == proj) { pindex = i; break; }
    if (pindex < 0) return;

    for (int i = 0; i < ws->num_docs; i++) {
        const ws_doc *w = &ws->docs[i];
        if (w->project_index != pindex || w->disk_only || !w->snap) continue;
        const doc_snapshot *s = w->snap;

        size_t uri_len = strlen(s->uri);
        int is_tji = uri_len >= 4 && strcmp(s->uri + (uri_len - 4), ".tji") == 0;
        if (is_tji) {
            /* An include fragment opened with no including .tjp is parsed in
             * isolation; warn once at the top of the file.
             *
             * TODO: offer an LSP code action to generate a compile_commands.json
             * for the user when it is missing for a .tjp that includes this
             * fragment. */
            LspRange r = { { 0, 0 }, { 0, (uint32_t)INT_MAX } };
            if (ws->cc_status == CC_STATUS_MALFORMED)
                add_warning(out, s->uri, r,
                    "Malformed compile_commands.json, this file will be parsed "
                    "stand-alone, not as part of any other loaded .tjp file "
                    "that includes it.");
            else
                add_warning(out, s->uri, r,
                    "Missing compile_commands.json, this file will be parsed "
                    "stand-alone, not as part of any other loaded .tjp file "
                    "that includes it.");
            continue;
        }

        /* .tjp: one warning per `include` directive, located on its
         * KW_INCLUDE token.  A .tjp with no includes loses nothing
         * cross-file and gets no warning. */
        for (int t = 0; t < s->num_tok_spans; t++) {
            if (s->tok_spans[t].token_kind != KW_INCLUDE) continue;
            LspRange r = { s->tok_spans[t].start, s->tok_spans[t].end };
            if (ws->cc_status == CC_STATUS_MALFORMED)
                add_warning(out, s->uri, r,
                    "Malformed compile_commands.json, cross-file LSP features "
                    "are disabled.");
            else
                add_warning(out, s->uri, r,
                    "Missing compile_commands.json, cross-file LSP features "
                    "are disabled.");
        }
    }
}

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
