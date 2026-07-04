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
 *  Best-effort filesystem write confinement for child processes (Linux
 *  Landlock).  Used to run the untrusted-project tj3 compiler so that a
 *  malicious report file path cannot write outside its temporary staging
 *  directory.  On any platform or kernel without Landlock these degrade to
 *  "unavailable", and the caller is expected to avoid generating reports
 *  instead of running an unconfined child. */

#pragma once

/**
 * Query whether the running kernel supports Landlock filesystem confinement.
 *
 * Only queries the Landlock ABI version; it does NOT confine the calling
 * process, so it is safe to call from the coordinator or a worker thread.
 * The result is computed once and cached.
 *
 * @return 1 if sandbox_confine_writes_to() can be expected to succeed, 0 if
 *         Landlock is unavailable (old kernel, disabled LSM, or non-Linux).
 */
int sandbox_available(void);

/**
 * Confine the CALLING thread so that filesystem writes are permitted only
 * beneath @p dir; reads and executes anywhere on the filesystem remain
 * allowed (so an interpreter like tj3/Ruby still loads its libraries).
 *
 * Intended to be called in a forked child between fork() and exec(): it uses
 * only async-signal-safe primitives (raw syscalls, open/close/prctl) and
 * performs no allocation or stdio.  The restriction is inherited across
 * execve() and cannot be lifted, which is why it must be applied in the child
 * rather than the server process.
 *
 * @param dir  Directory the child may write beneath (typically its cwd).
 * @return 0 on success (writes now confined); -1 if confinement could not be
 *         established.  A -1 return means the caller must abort the child
 *         (_exit) rather than exec an unconfined process.
 */
int sandbox_confine_writes_to(const char *dir);
