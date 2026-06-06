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

#ifndef DEBUG_H
#define DEBUG_H

/** @file
 *  Compile-time, per-category debug logging.
 *
 *  All output goes to stderr; stdout is reserved for the LSP JSON-RPC
 *  stream, which the client watches, so it must never be written to for
 *  debugging.  Most LSP clients surface a server's stderr in a dedicated
 *  output channel, which makes it the natural debug sink.
 *
 *  Each category below is an integer verbosity ceiling.  A value of 0
 *  disables the category entirely: its call sites compile to nothing —
 *  no branch, no string formatting, not even evaluation of the message
 *  arguments — so disabled logging has zero runtime cost.  Higher values
 *  admit progressively more verbose messages (see the LOG_* levels).
 *
 *  Raise a category by editing its default here, or override it at build
 *  time without touching this file:
 *
 *      make CFLAGS_EXTRA="-DDEBUG_TJ3=3 -DDEBUG_REVALIDATE=2"
 *
 *  Emit log lines with the DLOG() macro, passing the category macro name
 *  itself and a verbosity level:
 *
 *      DLOG(DEBUG_LIFECYCLE, LOG_INFO,  "initialize: root=%s", root);
 *      DLOG(DEBUG_PARSER,    LOG_TRACE, "token %d kind=%d", i, kind);
 */

#include <stdarg.h>

/* ── Verbosity levels (shared scale across all categories) ───────────────── */

#define LOG_INFO    1   /**< Key, low-frequency events (one line per operation). */
#define LOG_VERBOSE 2   /**< Detailed context: paths, counts, sizes, timings.   */
#define LOG_TRACE   3   /**< Very high frequency: per-token, per-job, per-edge.  */

/* ── Per-category verbosity ceilings (0 = disabled) ──────────────────────── */

#ifndef DEBUG_LIFECYCLE
#define DEBUG_LIFECYCLE 0       /**< initialize/initialized/shutdown, workspace root. */
#endif
#ifndef DEBUG_COMPILE_COMMANDS
#define DEBUG_COMPILE_COMMANDS 0 /**< compile_commands.json stat/status/entries.     */
#endif
#ifndef DEBUG_DOCSTORE
#define DEBUG_DOCSTORE 0        /**< didOpen/didChange/didClose/rename/watched files. */
#endif
#ifndef DEBUG_INCLUDES
#define DEBUG_INCLUDES 0        /**< include resolution and not-found / I/O errors.   */
#endif
#ifndef DEBUG_PARSER
#define DEBUG_PARSER 0          /**< parse pipeline, token/node counts.               */
#endif
#ifndef DEBUG_REVALIDATE
#define DEBUG_REVALIDATE 0      /**< revalidation cycle, snapshot swap, timing, dump. */
#endif
#ifndef DEBUG_TJ3
#define DEBUG_TJ3 0            /**< tj3 invocation, exit/duration, diagnostic parsing. */
#endif
#ifndef DEBUG_THREADS
#define DEBUG_THREADS 0        /**< coordinator/worker spawn, job queue, ctx pinning.  */
#endif
#ifndef DEBUG_RPC
#define DEBUG_RPC 0            /**< JSON-RPC messages in/out, methods, payload sizes.  */
#endif

/* ── Logging macro ───────────────────────────────────────────────────────── */

/**
 * Emit a log line when category @p cat is configured to admit verbosity
 * @p level.  Because @p cat expands to a compile-time constant, the whole
 * statement — including evaluation of the format string and its
 * arguments — is removed by the optimiser when the category is disabled
 * or set below @p level.
 *
 * Pass the category macro name itself (e.g. DEBUG_PARSER): it is both
 * compared as an integer (its value) and stringified (#cat) to tag the
 * emitted line.
 */
#define DLOG(cat, level, ...) \
    do { \
        if ((cat) >= (level)) \
            debug_logf(#cat, (level), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

/**
 * Backend for DLOG(); do not call directly.  Serialises concurrent
 * writers and prints one timestamped, thread- and source-tagged line to
 * stderr.  A trailing newline is appended automatically.
 *
 * @param category  Stringified category macro name (e.g. "DEBUG_PARSER").
 * @param level     The LOG_* verbosity the call site logged at.
 * @param file      Source file of the call site (__FILE__).
 * @param line      Source line of the call site (__LINE__).
 * @param fmt       printf-style format string.
 */
void debug_logf(const char *category, int level,
                const char *file, int line,
                const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#endif /* DEBUG_H */
