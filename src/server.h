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

#include "job_queue.h"

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
 * Capture a workspace snapshot, parse / clone / resolve as the
 * mutation requires, then commit the result under the docs-store
 * mutex.  Runs on the mutation worker thread.  Builds and emits any
 * response or notification via lsp_send_message().
 *
 * @param job  Pending mutation job.  Worker frees it after return.
 */
void server_dispatch_mutation(Job *job);

/**
 * Capture a workspace snapshot onto @p job and attach it.  Called by
 * the threadpool coordinator just before handing the job off to the
 * query worker pool — taking the snapshot here (in arrival order, with
 * docs_mutex briefly held) ensures the query observes exactly the
 * state that would have been visible if every job ran sequentially in
 * arrival order, even when later mutations race ahead while the query
 * is still in flight.
 *
 * @param job  Query job to snapshot.
 */
void server_capture_snapshot_for(Job *job);

/**
 * Run the query handler against the snapshot already attached to
 * @p job, then send the response.  Runs on any query worker thread.
 * Builds and emits the response via lsp_send_message().
 *
 * @param job  Pending query job.  Worker frees it after return.
 */
void server_dispatch_query(Job *job);

/**
 * Respond to @p job's request with a JSON-RPC `RequestCancelled`
 * (-32800) error without running the handler.  Called by the query
 * worker when the Job was marked is_cancelled by a $/cancelRequest
 * that arrived while the Job was still queued.  No-op for
 * notifications (no id to reply to).
 */
void server_dispatch_cancelled(Job *job);

/**
 * Write one LSP-framed message to stdout, prepending the required
 * `Content-Length` header.  Safe to call from any thread; serialized
 * internally by a mutex.
 *
 * @param msg  NUL-terminated JSON message body.
 */
void lsp_send_message(const char *msg);
