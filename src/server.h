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

#include <yyjson.h>

/**
 * Initialise the document store and any per-process state used by
 * server_process().  Must be called once before the first message is
 * processed.
 */
void server_init();

/**
 * Process one JSON-RPC request or notification arriving on the reader
 * thread.  Parses the envelope, classifies the method as a mutation
 * or a read-only query, and enqueues a Job onto the appropriate
 * thread-pool queue.  The `exit` notification is special-cased: the
 * thread pool is drained and the process terminates immediately.
 *
 * @param json_text  The raw JSON message body (without the
 *                   Content-Length framing).  Owned by the caller.
 */
void server_process(const char *json_text);

/**
 * Run a mutation / lifecycle JSON-RPC message under the doc-store
 * write lock.  Called only by the threadpool's mutation worker.
 * Builds and emits any response or notification via lsp_send_message().
 *
 * @param request_doc  Parsed JSON-RPC envelope.  Not freed by this
 *                     function; the worker frees it after return.
 */
void server_dispatch_mutation(yyjson_doc *request_doc);

/**
 * Run a read-only query JSON-RPC message under the doc-store read
 * lock.  Called by any threadpool query worker.  Builds and emits
 * the response via lsp_send_message().
 *
 * @param request_doc  Parsed JSON-RPC envelope.  Not freed by this
 *                     function; the worker frees it after return.
 */
void server_dispatch_query(yyjson_doc *request_doc);

/**
 * Respond to @p request_doc with a JSON-RPC `ContentModified` (-32801)
 * error without running the handler.  Called by the query worker when
 * a later same-URI mutation has overtaken this query, making its result
 * irrelevant to the client.  No-op for notifications (no id to reply to).
 */
void server_dispatch_stale(yyjson_doc *request_doc);

/**
 * Respond to @p request_doc with a JSON-RPC `RequestCancelled` (-32800)
 * error without running the handler.  Called by the query worker when
 * the Job was marked is_cancelled by a $/cancelRequest that arrived
 * before the worker started dispatching.  Also evicts the id from the
 * cancellation set so the fallback signal stays clean.  No-op for
 * notifications (no id to reply to).
 */
void server_dispatch_cancelled(yyjson_doc *request_doc);

/**
 * Write one LSP-framed message to stdout, prepending the required
 * `Content-Length` header.  Safe to call from any thread; serialized
 * internally by a mutex.
 *
 * @param msg  NUL-terminated JSON message body.
 */
void lsp_send_message(const char *msg);
