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

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum allowed length (in bytes) for a message (16 MB) */
#define CONTENT_LENGTH_MAX (16 * 1024 * 1024)

/* Read one LSP message from stdin.
 * Returns heap-allocated body string, or NULL on EOF/error.
 * Caller must free. */
static char *read_message(void) {
    int  content_length = -1;
    char line[256];

    /* Read headers */
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing \r\n */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
            line[--len] = '\0';

        if (len == 0) break; /* blank line = end of headers */

        if (strncmp(line, "Content-Length: ", 16) == 0)
            content_length = atoi(line + 16);
    }

    if (content_length <= 0 || content_length >= CONTENT_LENGTH_MAX)
        return NULL;

    char *buf = malloc((size_t)content_length + 1);
    if (!buf) return NULL;

    size_t total = 0;
    while (total < (size_t)content_length) {
        size_t n = fread(buf + total, 1, (size_t)content_length - total, stdin);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    return buf;
}

int main(void) {
    server_init();
    for (;;) {
        char *body = read_message();
        if (!body) break;

        char *response = server_process(body);
        free(body);

        if (response) {
            lsp_send_message(response);
            free(response);
        }
    }
    return 0;
}
