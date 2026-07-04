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

/* LSP lifecycle and document-sync handlers: initialize / initialized /
 * shutdown plus every state-mutating textDocument / workspace notification.
 * All of these run on the coordinator thread under docs_mutex (taken by the
 * dispatcher in server.c) and mutate the live document store; anything that
 * changes a document triggers revalidate_all_docs(). */

#pragma once

#include <yyjson.h>

/**
 * Handle the LSP "initialize" request: record the workspace root, and
 * return the server's capabilities and version.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  "initialize" params object containing "rootUri" and client
 *                capabilities; may be NULL.
 * @return        JSON-RPC response object with server capabilities.
 */
yyjson_mut_val *handle_initialize(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params);

/**
 * Handle the LSP "shutdown" request by returning a JSON null result.
 *
 * @param doc  Mutable document for building the response.
 * @param id   Request id from the incoming JSON-RPC message.
 * @return     JSON-RPC response object with a null result.
 */
yyjson_mut_val *handle_shutdown(yyjson_mut_doc *doc, yyjson_val *id);

/**
 * Handle the LSP "initialized" notification: register file-system watchers
 * for *.tjp and *.tji files via client/registerCapability, then load
 * compile_commands.json and trigger the initial revalidation.
 */
void handle_initialized(void);

/**
 * Handle the "textDocument/didOpen" notification: allocate or update the
 * document slot with the editor-supplied text, parse it, follow includes,
 * and trigger revalidation.
 *
 * @param params  JSON params with a "textDocument" object containing "uri"
 *                and "text".
 */
void handle_didopen(yyjson_val *params);

/**
 * Handle the "textDocument/didChange" notification: apply each content
 * change (incremental or full-replace) to the document's text, re-parse
 * the result, follow includes, and trigger revalidation.
 *
 * @param params  JSON params with "textDocument" and "contentChanges".
 */
void handle_didchange(yyjson_val *params);

/**
 * Handle the "textDocument/didClose" notification: revert the document to
 * a disk-only entry by re-reading the file from disk, or remove the slot
 * entirely if the file is no longer readable, then trigger revalidation.
 *
 * @param params  JSON params with a "textDocument" object containing "uri".
 */
void handle_didclose(yyjson_val *params);

/**
 * Handle the "workspace/didChangeWatchedFiles" notification: reload or
 * remove each changed disk-only document as indicated by its event type,
 * then revalidate if anything changed.
 *
 * @param params  JSON params object with a "changes" array of file events.
 */
void handle_did_change_watched_files(yyjson_val *params);

/**
 * Handle the "workspace/didRenameFiles" notification: remove the old
 * document entry (clearing its diagnostics if editor-managed), load
 * the renamed file from its new path, then revalidate if anything changed.
 *
 * @param params  JSON params object with a "files" array of rename pairs.
 */
void handle_did_rename_files(yyjson_val *params);
