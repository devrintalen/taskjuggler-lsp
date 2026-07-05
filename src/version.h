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

#pragma once

/**
 * @file version.h
 * @brief Build-time version constants embedded into the `initialize`
 * response's `serverInfo.version` field.  This file is the single source
 * of truth for the version: configure.ac extracts VERSION_STRING at
 * autoreconf time (see the release checklist in CLAUDE.md).
 */

#define VERSION_MAJOR 0           /**< Major component of the server version. */
#define VERSION_MINOR 5           /**< Minor component of the server version. */
#define VERSION_PATCH 3           /**< Patch component of the server version. */
#define VERSION_STRING "0.5.3"    /**< Dotted version string reported to clients. */
