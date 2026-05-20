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

#include "parser.h"
#include <yyjson.h>
#include <time.h>

/**
 * A named declaration (project, task, resource, account, shift) in the
 * document's symbol tree.
 *
 * Mirrors LSP DocumentSymbol.  Used for
 * textDocument/documentSymbol, completion identifiers, and dep validation.
 *
 * name is the human-readable label (the quoted string after the identifier in
 * TJP).  id is the short identifier used in dependency paths and cross-
 * references.  Both are heap-allocated.
 *
 * range covers the full declaration including its body; selection_range covers
 * only the identifier token, which is what the editor highlights on navigation.
 *
 * Example TJP input (task at line 0, subtask at line 2):
 *
 *   task spec "Specification" {     <- line 0
 *       task gui "GUI" {}           <- line 2
 *   }                               <- line 3
 *
 * Produces a DocSymbol tree:
 *
 *   DocSymbol {
 *     .name           = "Specification",
 *     .id         = "spec",
 *     .kind           = SK_FUNCTION,
 *     .range          = { {0,0}, {3,1} },   // full task block
 *     .selection_range= { {0,5}, {0,9} },   // just "spec"
 *     .num_children   = 1,
 *     .children       = [
 *       DocSymbol {
 *         .name           = "GUI",
 *         .id         = "gui",
 *         .kind           = SK_FUNCTION,
 *         .range          = { {2,4}, {2,22} },
 *         .selection_range= { {2,9}, {2,12} },
 *         .num_children   = 0,
 *       }
 *     ]
 *   }
 */
struct DocSymbol {
    char      *name;           /**< display name, heap-allocated */
    char      *id;             /**< TJP identifier, heap-allocated */
    int        keyword;        /**< KW_* constant from grammar.tab.h */
    LspRange   range;          /**< full declaration range, including body */
    LspRange   selection_range;/**< range of just the identifier token */
    /* Date attributes parsed from the body.  Meaningful only when
     * keyword == KW_TASK; otherwise has_start/has_end remain 0.
     * Populated by grammar actions for `start <date>` / `end <date>`. */
    time_t     start_date;     /**< explicit `start` date, valid if has_start */
    time_t     end_date;       /**< explicit `end` date, valid if has_end */
    int        has_start;      /**< 1 if `start_date` is populated */
    int        has_end;        /**< 1 if `end_date` is populated */
    DocSymbol *parent;         /**< parent node; NULL for root-level symbols */
    DocSymbol **children;      /**< array of pointers to heap-allocated children */
    int        num_children;   /**< number of entries in children */
    int        children_cap;   /**< allocated capacity of children */
    DefinitionLink *def_links; /**< outgoing resolved references */
    int        num_def_links;  /**< number of entries in def_links */
    int        def_links_cap;  /**< allocated capacity of def_links */
    ReferenceLink *ref_links;  /**< incoming references from other symbols */
    int        num_ref_links;  /**< number of entries in ref_links */
    int        ref_links_cap;  /**< allocated capacity of ref_links */
};

/**
 * A resolved outgoing reference stored on the DocSymbol that declared the dependency.
 *
 * source is the range of the reference expression (e.g. a dependency path),
 * target points to the DocSymbol being referred to.
 *
 * Populated by revalidate_dep_refs() for every successfully resolved
 * dependency reference.  Each link is owned by the declaring DocSymbol
 * (the task containing the `depends` or `precedes` clause).
 *
 * target_uri is heap-allocated and is NULL when the target is in the same
 * document as the source.  For cross-file references it holds the URI of
 * the file that defines the target symbol.
 *
 * Example TJP input:
 *
 *   task database "Database" {}     <- line 0; "database" is at {0,5}..{0,13}
 *   task gui "GUI" {
 *       depends database            <- line 2; "database" is at {2,16}..{2,24}
 *   }
 *
 * After revalidate_dep_refs() resolves the dependency, gui's def_links[]
 * contains:
 *
 *   DefinitionLink {
 *     .source     = { {2,16}, {2,24} },   // range of "database" in depends expr
 *     .target     = &database_sym,         // resolved target DocSymbol
 *     .target_uri = NULL,                  // same document
 *   }
 *
 * When the user invokes go-to-definition with the cursor anywhere in source,
 * the server finds this link and jumps the editor to the target's
 * selection_range.
 */
struct DefinitionLink {
    LspRange   source;     /**< range of the reference expression */
    DocSymbol *target;     /**< resolved target symbol */
    char      *target_uri; /**< heap-allocated; NULL means same document */
};

/**
 * An incoming reference to a DocSymbol — the inverse of DefinitionLink.
 *
 * While DefinitionLink lives on the *declaring* symbol and points outward
 * to the target, ReferenceLink lives on the *target* symbol and points
 * back to each site that references it.
 *
 * source is the range of the reference expression at the call site.
 * origin points to the DocSymbol that made the reference (the owner of
 * the corresponding DefinitionLink).
 *
 * source_uri is heap-allocated and is NULL when the reference originates
 * from the same document as the target.  For cross-file references it
 * holds the URI of the file containing the reference.
 *
 * Example TJP input:
 *
 *   task database "Database" {}     <- line 0
 *   task gui "GUI" {
 *       depends database            <- line 2; "database" is at {2,16}..{2,24}
 *   }
 *
 * After resolution, database's ref_links[] contains:
 *
 *   ReferenceLink {
 *     .source     = { {2,16}, {2,24} },   // range of "database" in depends expr
 *     .origin     = &gui_sym,              // the symbol that declared the dep
 *     .source_uri = NULL,                  // same document
 *   }
 */
struct ReferenceLink {
    LspRange   source;     /**< range of the reference expression */
    DocSymbol *origin;     /**< symbol that contains the reference */
    char      *source_uri; /**< heap-allocated; NULL means same document */
};

/**
 * Recursively free a DocSymbol and all of its children.
 *
 * @param s  Root of the DocSymbol subtree to free.
 */
void doc_symbol_free(DocSymbol *s);

/**
 * Return the innermost DocSymbol whose range contains @p pos (inclusive
 * start, exclusive end), using the precomputed `.owner` field on
 * @p tokens.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param pos         Position to look up.
 * @return The matching symbol, or NULL when @p pos is outside every
 *         DocSymbol.  Runs in O(log T + D) where T is @p num_tokens
 *         and D is the symbol nesting depth at @p pos.
 */
DocSymbol *symbol_at(const TokenSpan *tokens, int num_tokens, LspPos pos);

/**
 * Navigate the DocSymbol tree by following @p path (an array of @p plen
 * identifier strings).
 *
 * Only SK_FUNCTION nodes are traversed (i.e. tasks).
 *
 * @param syms   Top-level symbols.
 * @param n      Length of @p syms.
 * @param path   Identifiers to follow, outermost first.
 * @param plen   Length of @p path.
 * @param out_n  Receives the length of the returned children array.
 * @return Children array of the matched node, or NULL with `*out_n = 0`
 *         when the path does not resolve.
 */
DocSymbol *const *doc_symbol_find_path(DocSymbol *const *syms, int n,
                                       const char **path, int plen,
                                       int *out_n);

/**
 * Serialise an LspRange to a mutable JSON object allocated in @p doc.
 *
 * @param doc  Destination mutable JSON document.
 * @param r    Range to serialise.
 * @return The newly created JSON object.
 */
yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r);

/**
 * Map a KW_* keyword constant to the corresponding LSP SymbolKind (SK_*).
 *
 * @param keyword  KW_* constant from grammar.tab.h.
 * @return The matching LSP SymbolKind value.
 */
int symbol_kind_for(int keyword);

/**
 * Serialise the documentSymbol tree to a heap-allocated, NUL-terminated JSON
 * array string.  Intended to be cached and later embedded via
 * yyjson_mut_rawncpy.
 *
 * @param syms     Top-level symbols of the document.
 * @param n        Length of @p syms.
 * @param out_len  Receives the byte length of the result, excluding the NUL.
 * @return Heap-allocated JSON; caller owns and must free().
 */
char *build_document_symbols_json(DocSymbol *const *syms, int n, size_t *out_len);
