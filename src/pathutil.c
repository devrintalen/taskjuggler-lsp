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

#include "pathutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Decode a percent-encoded string, replacing %XX escape sequences with
 *  the corresponding byte.
 *  @param src  Null-terminated percent-encoded string.
 *  @return     Freshly allocated decoded string; caller must free. */
static char *percent_decode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    size_t wi = 0;
    for (size_t ri = 0; ri < len; ri++) {
        if (src[ri] == '%' && ri + 2 < len
                && isxdigit((unsigned char)src[ri + 1])
                && isxdigit((unsigned char)src[ri + 2])) {
            char hex[3] = { src[ri + 1], src[ri + 2], '\0' };
            dst[wi++] = (char)strtol(hex, NULL, 16);
            ri += 2;
        } else {
            dst[wi++] = src[ri];
        }
    }
    dst[wi] = '\0';
    return dst;
}

/** Percent-encode a filesystem path for use in a file:// URI, preserving
 *  '/' separators and unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~').
 *  @param src  Null-terminated path string.
 *  @return     Freshly allocated encoded string; caller must free. */
static char *percent_encode_path(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len * 3 + 1);
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    size_t wi = 0;
    for (size_t ri = 0; ri < len; ri++) {
        unsigned char c = (unsigned char)src[ri];
        if (c == '/'
                || (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[wi++] = (char)c;
        } else {
            dst[wi++] = '%';
            dst[wi++] = "0123456789ABCDEF"[c >> 4];
            dst[wi++] = "0123456789ABCDEF"[c & 0xf];
        }
    }
    dst[wi] = '\0';
    return dst;
}

char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    return percent_decode(uri + 7);
}

char *path_to_uri(const char *path) {
    char *encoded = percent_encode_path(path);
    size_t enc_len = strlen(encoded);
    char *uri = malloc(7 + enc_len + 1);
    if (!uri) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(uri, "file://", 7);
    memcpy(uri + 7, encoded, enc_len + 1);
    free(encoded);
    return uri;
}

/** Lexically normalize @p path by collapsing redundant separators and
 *  removing single-dot segments, without consulting the filesystem.
 *  Used as a fallback when realpath() fails.
 *  @param path  Null-terminated filesystem path.
 *  @return      Freshly allocated normalized path, or NULL on allocation failure. */
static char *lexical_normalize_path(const char *path) {
    if (!path) return NULL;
    size_t len = strlen(path);
    char *out = malloc(len + 2);
    if (!out) return NULL;

    int absolute = (len > 0 && path[0] == '/');
    size_t wi = 0;
    if (absolute) out[wi++] = '/';

    size_t i = 0;
    int wrote_segment = 0;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) break;
        size_t seg_start = i;
        while (i < len && path[i] != '/') i++;
        size_t seg_len = i - seg_start;
        if (seg_len == 1 && path[seg_start] == '.') continue;
        if (wrote_segment) out[wi++] = '/';
        memcpy(out + wi, path + seg_start, seg_len);
        wi += seg_len;
        wrote_segment = 1;
    }
    out[wi] = '\0';
    return out;
}

char *normalize_uri(const char *raw_uri) {
    if (!raw_uri) return NULL;
    if (strncmp(raw_uri, "file://", 7) != 0) return strdup(raw_uri);

    char *path = uri_to_path(raw_uri);
    if (!path) return strdup(raw_uri);

    char *canon = realpath(path, NULL);
    if (!canon) canon = lexical_normalize_path(path);
    free(path);
    if (!canon) return NULL;

    char *uri = path_to_uri(canon);
    free(canon);
    return uri;
}
