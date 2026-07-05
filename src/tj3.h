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
 *  Runs the real `tj3` compiler against one assembled project and turns its
 *  stderr diagnostics into LSP Diagnostics.  Because there is no assembled
 *  project text in memory (each document's source lives in its own
 *  doc_snapshot), the runner materialises the project's member documents into
 *  a temporary directory — preserving paths relative to their common ancestor
 *  so `include` directives resolve — invokes tj3 there, parses stderr, and
 *  removes the directory.  This is run off the coordinator on a per-project
 *  diagnostics worker (see diag_worker.h). */

#pragma once

#include "query_context.h"
#include "diagnostics.h"

/** Which tj3 invocation to use for a project's worker class. */
typedef enum tj3_mode {
    TJ3_FULL,          /**< schedules and generates reports, so scheduling and
                            report errors surface too.  Report generation writes
                            files at project-controlled paths, so the run is
                            confined to its staging dir via Landlock; without
                            Landlock it degrades to `--no-reports`. */
    TJ3_SYNTAX_ONLY    /**< `tj3 --check-syntax <root>`: parse-level validation only */
} tj3_mode;

/**
 * Run tj3 against the single project @p proj within snapshot @p ws, adding any
 * diagnostics it reports into @p out (keyed by document URI).  Diagnostics for
 * files outside the project are dropped.  No-op if tj3 is not on PATH or the
 * project cannot be materialised.
 *
 * @param ws    Workspace snapshot providing the member documents' text.
 * @param proj  Project whose root document is fed to tj3.
 * @param mode  Whether to run full scheduling or `--check-syntax` only.
 * @param out   Output diag_set; new diagnostics are appended.
 */
void tj3_collect_project(const workspace_snapshot *ws, const ws_project *proj,
                         tj3_mode mode, diag_set *out);
