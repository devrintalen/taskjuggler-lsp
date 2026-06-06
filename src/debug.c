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

#include "debug.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/** Serialises debug output so concurrent threads (coordinator, query
 *  workers, diagnostics workers) cannot interleave a single line.  Has
 *  its own lock and never touches stdout / stdout_mutex. */
static pthread_mutex_t debug_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Human-readable name for a LOG_* verbosity level.
 *  @param level  One of the LOG_* constants.
 *  @return       Static string naming the level; never NULL. */
static const char *level_name(int level) {
    switch (level) {
    case LOG_INFO:    return "INFO";
    case LOG_VERBOSE: return "VERBOSE";
    case LOG_TRACE:   return "TRACE";
    default:          return "LOG";
    }
}

void debug_logf(const char *category, int level,
                const char *file, int line,
                const char *fmt, ...) {
    /* Wall-clock timestamp with millisecond resolution. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);

    /* Drop the "DEBUG_" prefix and any source directory for compactness. */
    const char *cat = category;
    if (strncmp(cat, "DEBUG_", 6) == 0) cat += 6;
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/') base = p + 1;

    pthread_mutex_lock(&debug_mutex);
    fprintf(stderr, "[%s.%03ld] [tid %lu] [%s/%s] %s:%d: ",
            timebuf, ts.tv_nsec / 1000000L,
            (unsigned long)pthread_self(),
            cat, level_name(level), base, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&debug_mutex);
}
