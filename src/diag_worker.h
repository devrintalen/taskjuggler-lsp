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

/** @file
 *  Asynchronous tj3 diagnostics, one worker thread per project.
 *
 *  Running tj3 inline on the coordinator would stall the server, so each
 *  project gets its own long-lived worker that runs tj3 against an immutable
 *  workspace snapshot off the coordinator and publishes the result.  Workers
 *  come in two classes keyed on compile_commands.json membership: full `tj3`
 *  (scheduling + syntax) for compile_commands projects, `tj3 --check-syntax`
 *  for orphan editor-only documents.
 *
 *  Each worker coalesces: while it is busy, newer snapshots replace its
 *  pending request rather than queuing, so bursts of edits collapse into as
 *  few tj3 runs as possible and the latest snapshot is always eventually
 *  validated.  A slow project never delays another project's diagnostics.
 *
 *  The registry is owned by the coordinator and is not thread-safe; call both
 *  functions only from the coordinator (or, for shutdown, after the thread
 *  pool has been joined). */

#pragma once

#include "query_context.h"

/**
 * Reconcile the worker set against @p ws: spawn a worker (of the right class)
 * for every project in the snapshot, retire workers whose project disappeared,
 * and hand @p ws to each surviving worker as its newest pending request.
 */
void diag_registry_update(workspace_snapshot *ws);

/** Stop and join every worker, clearing their published diagnostics. */
void diag_registry_shutdown(void);
