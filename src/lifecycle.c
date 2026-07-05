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

#include "lifecycle.h"
#include "document_store.h"
#include "workspace.h"
#include "diagnostics.h"
#include "semantic_tokens.h"
#include "pathutil.h"
#include "rpc.h"
#include "debug.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════════
   initialize / initialized / shutdown
   ═══════════════════════════════════════════════════════════════════════════ */

/** Build the server "capabilities" object advertised in the initialize
 *  response: text-document sync options, the per-feature provider flags,
 *  the semantic-token legend, and the workspace file-operation filters.
 *  Kept separate so handle_initialize stays focused on lifecycle setup.
 *  @param doc  Mutable document the capabilities tree is allocated in.
 *  @return     The populated "capabilities" object. */
static yyjson_mut_val *build_server_capabilities(yyjson_mut_doc *doc) {
    yyjson_mut_val *comp_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, comp_triggers, "!");
    yyjson_mut_arr_add_str(doc, comp_triggers, ".");
    yyjson_mut_val *comp_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc,  comp_opts, "triggerCharacters", comp_triggers);
    yyjson_mut_obj_add_bool(doc, comp_opts, "resolveProvider", false);

    yyjson_mut_val *sig_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, sig_triggers, " ");
    yyjson_mut_val *sig_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sig_opts, "triggerCharacters", sig_triggers);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tds = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, tds, "change", 2);
    yyjson_mut_obj_add_bool(doc, tds, "openClose", true);
    yyjson_mut_val *sem_types = yyjson_mut_arr(doc);
    for (int i = 0; i < num_semantic_token_types; i++)
        yyjson_mut_arr_add_str(doc, sem_types, semantic_token_type_names[i]);
    yyjson_mut_val *sem_mods = yyjson_mut_arr(doc);
    for (int i = 0; i < num_semantic_token_modifiers; i++)
        yyjson_mut_arr_add_str(doc, sem_mods, semantic_token_modifier_names[i]);
    yyjson_mut_val *sem_legend = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sem_legend, "tokenTypes",     sem_types);
    yyjson_mut_obj_add_val(doc, sem_legend, "tokenModifiers", sem_mods);
    yyjson_mut_val *sem_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc,  sem_opts, "legend", sem_legend);
    yyjson_mut_val *sem_full = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, sem_full, "delta", true);
    yyjson_mut_obj_add_val(doc, sem_opts, "full", sem_full);

    yyjson_mut_obj_add_val(doc,  caps, "textDocumentSync",          tds);
    yyjson_mut_obj_add_bool(doc, caps, "documentSymbolProvider",    true);
    yyjson_mut_obj_add_bool(doc, caps, "foldingRangeProvider",      true);
    yyjson_mut_val *code_lens_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, code_lens_opts, "resolveProvider", false);
    yyjson_mut_obj_add_val(doc,  caps, "codeLensProvider",          code_lens_opts);
    yyjson_mut_obj_add_bool(doc, caps, "hoverProvider",             true);
    yyjson_mut_obj_add_bool(doc, caps, "definitionProvider",        true);
    yyjson_mut_obj_add_bool(doc, caps, "referencesProvider",        true);
    yyjson_mut_obj_add_bool(doc, caps, "documentHighlightProvider", true);
    yyjson_mut_obj_add_val(doc,  caps, "signatureHelpProvider",     sig_opts);
    yyjson_mut_obj_add_val(doc,  caps, "completionProvider",        comp_opts);
    yyjson_mut_obj_add_val(doc,  caps, "semanticTokensProvider",    sem_opts);
    yyjson_mut_obj_add_bool(doc, caps, "workspaceSymbolProvider",   true);

    yyjson_mut_val *rename_filter_tjp = yyjson_mut_obj(doc);
    yyjson_mut_val *pattern_tjp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, pattern_tjp, "glob", "**/*.tjp");
    yyjson_mut_obj_add_val(doc, rename_filter_tjp, "pattern", pattern_tjp);
    yyjson_mut_val *rename_filter_tji = yyjson_mut_obj(doc);
    yyjson_mut_val *pattern_tji = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, pattern_tji, "glob", "**/*.tji");
    yyjson_mut_obj_add_val(doc, rename_filter_tji, "pattern", pattern_tji);
    yyjson_mut_val *rename_filters = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(rename_filters, rename_filter_tjp);
    yyjson_mut_arr_add_val(rename_filters, rename_filter_tji);
    yyjson_mut_val *did_rename_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, did_rename_opts, "filters", rename_filters);
    yyjson_mut_val *file_ops = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, file_ops, "didRename", did_rename_opts);
    yyjson_mut_val *workspace_folders_caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, workspace_folders_caps, "supported", false);
    yyjson_mut_val *workspace_caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, workspace_caps, "workspaceFolders", workspace_folders_caps);
    yyjson_mut_obj_add_val(doc, workspace_caps, "fileOperations",   file_ops);
    yyjson_mut_obj_add_val(doc, caps, "workspace", workspace_caps);

    return caps;
}

yyjson_mut_val *handle_initialize(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params) {
    if (params) {
        yyjson_val *root_uri_val = yyjson_obj_get(params, "rootUri");
        if (root_uri_val && yyjson_is_str(root_uri_val))
            workspace_set_root_from_uri(yyjson_get_str(root_uri_val));
    }

    DLOG(DEBUG_LIFECYCLE, LOG_INFO, "initialize: workspace_root=%s cc_path=%s",
         workspace_root() ? workspace_root() : "(none)",
         workspace_cc_path() ? workspace_cc_path() : "(none)");

#if DEBUG_LIFECYCLE
    yyjson_val *trace_val = params ? yyjson_obj_get(params, "trace") : NULL;
    if (trace_val && yyjson_is_str(trace_val))
        DLOG(DEBUG_LIFECYCLE, LOG_VERBOSE, "initialize: client trace=%s",
             yyjson_get_str(trace_val));
#endif

    yyjson_mut_val *server_info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, server_info, "name",    "taskjuggler-lsp");
    yyjson_mut_obj_add_str(doc, server_info, "version", VERSION_STRING);

    yyjson_mut_val *caps = build_server_capabilities(doc);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, result, "capabilities", caps);
    yyjson_mut_obj_add_val(doc, result, "serverInfo",   server_info);

    return make_response(doc, id, result);
}

yyjson_mut_val *handle_shutdown(yyjson_mut_doc *doc, yyjson_val *id) {
    DLOG(DEBUG_LIFECYCLE, LOG_INFO, "shutdown requested");
    return make_response(doc, id, yyjson_mut_null(doc));
}

void handle_initialized(void) {
    DLOG(DEBUG_LIFECYCLE, LOG_INFO,
         "initialized: registering file watchers, loading compile_commands");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *watcher_tjp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, watcher_tjp, "globPattern", "**/*.tjp");
    yyjson_mut_val *watcher_tji = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, watcher_tji, "globPattern", "**/*.tji");
    yyjson_mut_val *watchers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(watchers, watcher_tjp);
    yyjson_mut_arr_add_val(watchers, watcher_tji);

    yyjson_mut_val *register_options = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, register_options, "watchers", watchers);

    yyjson_mut_val *registration = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, registration, "id",     "file-watcher");
    yyjson_mut_obj_add_str(doc, registration, "method", "workspace/didChangeWatchedFiles");
    yyjson_mut_obj_add_val(doc, registration, "registerOptions", register_options);

    yyjson_mut_val *registrations = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(registrations, registration);

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, params, "registrations", registrations);

    yyjson_mut_val *request = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, request, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, request, "id",      "watcher-reg");
    yyjson_mut_obj_add_str(doc, request, "method",  "client/registerCapability");
    yyjson_mut_obj_add_val(doc, request, "params",  params);

    yyjson_mut_doc_set_root(doc, request);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    lsp_send_message(text);
    free(text);

    /* compile_commands.json is now the only docs[] populator at startup
     * (besides editor didOpen events).  follow_includes() inside
     * load_file_from_disk() still cascades into the .tji closure of each
     * listed .tjp; nothing else is pulled in from the workspace tree. */
    reload_compile_commands();
    if (workspace_root()) revalidate_all_docs();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Incremental text-change application
   ═══════════════════════════════════════════════════════════════════════════ */

/** Apply a single LSP incremental text change to @p src: replace the
 *  substring covered by @p range with @p new_text and return the
 *  resulting string.
 *  @param src       Original document text; treated as empty when NULL.
 *  @param range     The line/character range to replace.
 *  @param new_text  Replacement text to splice in.
 *  @return          Freshly allocated updated string, or NULL on allocation
 *                   failure. */
static char *apply_incremental_change(const char *src,
                                      LspRange range,
                                      const char *new_text) {
    if (!src) src = "";
    size_t src_len = strlen(src);
    size_t new_len = strlen(new_text);

    size_t start_byte = 0;
    uint32_t cur_line = 0, cur_char = 0;
    while (start_byte < src_len) {
        if (cur_line == range.start.line && cur_char == range.start.character)
            break;
        if (src[start_byte] == '\n') { cur_line++; cur_char = 0; }
        else                         { cur_char++; }
        start_byte++;
    }

    size_t end_byte = start_byte;
    cur_line = range.start.line;
    cur_char = range.start.character;
    while (end_byte < src_len) {
        if (cur_line == range.end.line && cur_char == range.end.character)
            break;
        if (src[end_byte] == '\n') { cur_line++; cur_char = 0; }
        else                       { cur_char++; }
        end_byte++;
    }

    size_t suffix_len = src_len - end_byte;
    char *result = malloc(start_byte + new_len + suffix_len + 1);
    if (!result) return NULL;
    memcpy(result,                        src,      start_byte);
    memcpy(result + start_byte,           new_text, new_len);
    memcpy(result + start_byte + new_len, src + end_byte, suffix_len);
    result[start_byte + new_len + suffix_len] = '\0';
    return result;
}

/** Fold an LSP "contentChanges" array onto @p base_text, applying each
 *  incremental (or full-replace) change in arrival order.
 *
 *  @p base_text is read but never freed; the result is a freshly allocated
 *  buffer the caller owns. @p base_text starts as NULL until the first change
 *  produces a buffer, so the first change reads straight from the document's
 *  current text — avoiding an upfront full-source strdup of a possibly
 *  multi-MB document on every keystroke.
 *
 *  @param base_text  Current document text the first change reads from; read
 *                    but never freed (may be NULL).
 *  @param changes    LSP "contentChanges" array to apply in arrival order.
 *  @return New buffer with all changes applied, or NULL when no usable change
 *          was produced (a change object missing "text", an allocation
 *          failure, or an empty change list). On NULL the caller must leave
 *          the document text untouched. */
static char *apply_content_changes(const char *base_text, yyjson_val *changes) {
    char *current = NULL;
    size_t idx, max;
    yyjson_val *change;
    yyjson_arr_foreach(changes, idx, max, change) {
        const char *base = current ? current : (base_text ? base_text : "");
        yyjson_val *range_obj = yyjson_obj_get(change, "range");
        const char *new_text  = yyjson_get_str(yyjson_obj_get(change, "text"));
        if (!new_text) { free(current); return NULL; }

        char *next;
        if (range_obj) {
            LspRange range;
            range.start = json_to_pos(yyjson_obj_get(range_obj, "start"));
            range.end   = json_to_pos(yyjson_obj_get(range_obj, "end"));
            next = apply_incremental_change(base, range, new_text);
        } else {
            next = strdup(new_text);
        }
        free(current);              /* NULL on the first iteration: a no-op */
        if (!next) return NULL;     /* base_text untouched; nothing leaked */
        current = next;
    }
    return current;
}

/* ═══════════════════════════════════════════════════════════════════════════
   textDocument sync notifications
   ═══════════════════════════════════════════════════════════════════════════ */

void handle_didopen(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri  = json_str(tdi, "uri");
    const char *text = json_str(tdi, "text");
    if (!uri || !text) return;

    DLOG(DEBUG_DOCSTORE, LOG_INFO, "didOpen: %s (%zu bytes)", uri, strlen(text));

    Document *d = doc_find(uri);
    if (d) {
        if (!d->disk_only) return; /* duplicate open: client error */
        if (d->text && strcmp(d->text, text) == 0) {
            d->disk_only = 0;
            revalidate_all_docs();
            return;
        }
        free(d->text);
        d->text = NULL;
        doc_clear_parse_state(d);
    } else {
        d = doc_alloc(uri);
        if (!d) return;
    }
    d->disk_only = 0;
    d->text  = strdup(text);
    ParseOutput *po = parse(text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, po);
        free(path);
    }
    doc_install_parse(d, po);

    revalidate_all_docs();
}

void handle_didchange(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi     = yyjson_obj_get(params, "textDocument");
    yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
    if (!tdi || !changes || !yyjson_is_arr(changes)) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    if (yyjson_arr_size(changes) == 0) return;

    DLOG(DEBUG_DOCSTORE, LOG_INFO, "didChange: %s (%zu change(s))",
         uri, yyjson_arr_size(changes));

    Document *d = doc_find(uri);
    if (!d) return;

    char *current = apply_content_changes(d->text, changes);
    if (!current) return;           /* no usable change applied; d->text kept */

    free(d->text);
    d->text = current;

    /* didChange runs serially on the coordinator, so its cost lands as latency
     * on the next request.  Time each phase (parse dominates) under
     * DEBUG_DOCSTORE >= LOG_VERBOSE; compiled out of the default build. */
#if DEBUG_DOCSTORE >= LOG_VERBOSE
    struct timespec dc_t[5];
    clock_gettime(CLOCK_MONOTONIC, &dc_t[0]);
#endif
    ParseOutput *po = parse(d->text);
#if DEBUG_DOCSTORE >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &dc_t[1]);
#endif

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, po);
        free(path);
    }
#if DEBUG_DOCSTORE >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &dc_t[2]);
#endif
    doc_install_parse(d, po);
#if DEBUG_DOCSTORE >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &dc_t[3]);
#endif

    revalidate_all_docs();
#if DEBUG_DOCSTORE >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &dc_t[4]);
    #define DC_MS(i, j) (((dc_t[j].tv_sec  - dc_t[i].tv_sec)  * 1000.0) \
                       +  ((dc_t[j].tv_nsec - dc_t[i].tv_nsec) / 1e6))
    DLOG(DEBUG_DOCSTORE, LOG_VERBOSE,
         "didChange phases: parse=%.1f follow_includes=%.1f install=%.1f "
         "revalidate=%.1f total=%.1f ms",
         DC_MS(0, 1), DC_MS(1, 2), DC_MS(2, 3), DC_MS(3, 4), DC_MS(0, 4));
    #undef DC_MS
#endif
}

void handle_didclose(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    DLOG(DEBUG_DOCSTORE, LOG_INFO, "didClose: %s", uri);

    Document *d = doc_find(uri);
    if (!d) return;

    char *path = uri_to_path(uri);
    char *text = path ? read_file(path) : NULL;

    if (text) {
        install_disk_text(d, text, path);
    } else {
        publish_diagnostics(uri);
        doc_free(d);
    }
    free(path);

    revalidate_all_docs();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Watched-file and rename notifications
   ═══════════════════════════════════════════════════════════════════════════ */

/** Drop a background (disk_only) document in response to a watched-file
 *  delete event. Editor-managed documents are left for didClose to handle.
 *  @param uri  URI of the deleted file.
 *  @return 1 if a document was removed from docs[], 0 otherwise. */
static int forget_watched_file(const char *uri) {
    Document *document = doc_find(uri);
    if (document && document->disk_only) {
        doc_free(document);
        return 1;
    }
    return 0;
}

/** Load or reload a watched file into a background (disk_only) slot in
 *  response to a create/change event. Files the editor already manages
 *  (non-disk_only) are left untouched so editor content stays authoritative.
 *  @param uri  URI of the created/changed file.
 *  @return 1 if docs[] changed, 0 otherwise. */
static int admit_watched_file(const char *uri) {
    Document *document = doc_find(uri);
    if (document && !document->disk_only) return 0;

    char *path = uri_to_path(uri);
    if (!path) return 0;
    char *text = read_file(path);
    if (!text) { free(path); return 0; }

    if (!document) document = doc_alloc(uri);
    if (!document) { free(text); free(path); return 0; }

    install_disk_text(document, text, path);
    free(path);
    return 1;
}

void handle_did_change_watched_files(yyjson_val *params) {
    if (!params) return;
    yyjson_val *changes = yyjson_obj_get(params, "changes");
    if (!changes || !yyjson_is_arr(changes)) return;

    int changed = 0;

    size_t idx, max;
    yyjson_val *event;
    yyjson_arr_foreach(changes, idx, max, event) {
        const char *uri = json_str(event, "uri");
        yyjson_val *type_item = yyjson_obj_get(event, "type");
        if (!uri || !type_item || !yyjson_is_num(type_item)) continue;
        int type = (int)yyjson_get_num(type_item);

        DLOG(DEBUG_DOCSTORE, LOG_VERBOSE,
             "watched file event: type=%d %s", type, uri);

        /* WatchKind 3 is Deleted; Created (1) and Changed (2) both reload. */
        if (type == 3)
            changed |= forget_watched_file(uri);
        else
            changed |= admit_watched_file(uri);
    }

    if (changed) revalidate_all_docs();
}

void handle_did_rename_files(yyjson_val *params) {
    if (!params) return;
    yyjson_val *files = yyjson_obj_get(params, "files");
    if (!files || !yyjson_is_arr(files)) return;

    int changed = 0;

    size_t idx, max;
    yyjson_val *file_item;
    yyjson_arr_foreach(files, idx, max, file_item) {
        const char *old_uri = json_str(file_item, "oldUri");
        const char *new_uri = json_str(file_item, "newUri");
        if (!old_uri || !new_uri) continue;

        DLOG(DEBUG_DOCSTORE, LOG_INFO, "rename: %s -> %s", old_uri, new_uri);

        Document *old_doc = doc_find(old_uri);
        if (old_doc) {
            if (!old_doc->disk_only)
                publish_diagnostics(old_uri);
            doc_free(old_doc);
            changed = 1;
        }

        char *path = uri_to_path(new_uri);
        if (!path) continue;
        char *text = read_file(path);
        if (!text) { free(path); continue; }

        Document *new_doc = doc_find(new_uri);
        if (!new_doc) new_doc = doc_alloc(new_uri);
        if (!new_doc) { free(text); free(path); continue; }

        install_disk_text(new_doc, text, path);
        free(path);
        changed = 1;
    }

    if (changed) revalidate_all_docs();
}
