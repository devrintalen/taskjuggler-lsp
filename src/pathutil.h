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
 *  Shared file:// <-> filesystem-path helpers.  Modules that need to
 *  translate URIs — e.g. the document store in server.c or the tj3
 *  diagnostics runner — include this header rather than duplicating the
 *  percent-encoding logic. */

#pragma once

/**
 * Convert a file:// URI to a filesystem path.
 *
 * @param uri  Source URI; must begin with "file://" to produce a result.
 * @return Heap-allocated percent-decoded filesystem path (owned by caller),
 *         or NULL when @p uri is not a file:// URI.
 */
char *uri_to_path(const char *uri);

/**
 * Convert a filesystem path to a percent-encoded file:// URI.
 *
 * @param path  Filesystem path to encode.
 * @return Heap-allocated URI (owned by caller).
 */
char *path_to_uri(const char *path);

/**
 * Canonicalize @p raw_uri: decode percent-encoding, resolve the path
 * through realpath() (falling back to a lexical normalization that
 * collapses redundant separators and single-dot segments), and re-encode
 * the result as a file:// URI.  Non-file URIs are returned unchanged
 * (duplicated).
 *
 * @param raw_uri  The raw URI string to normalize; may be NULL.
 * @return Freshly allocated canonical URI string (owned by caller), or NULL
 *         on allocation failure or a NULL input.
 */
char *normalize_uri(const char *raw_uri);
