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

#include "dependency.h"
#include "document_symbol.h"  /* tj_node_at */
#include "grammar.tab.h"      /* KW_TASK */

/* ── Cursor lookup ──────────────────────────────────────────────────────── */

static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

int dependency_at_cursor(const parse_slab *slab,
                         const TokenSpan *tokens, int num_tokens,
                         LspPos cursor,
                         tj_node **out_owner, const Dependency **out_dep) {
    for (tj_node *node = tj_node_at(slab, tokens, num_tokens, cursor);
         node != NULL; node = slab_node(slab, node->parent_node)) {
        Dependency *deps = slab_deps(slab, node);
        for (int i = 0; i < node->num_dependencies; i++) {
            if (pos_in_range(cursor, deps[i].source_range)) {
                if (out_owner) *out_owner = node;
                if (out_dep)   *out_dep   = &deps[i];
                return 1;
            }
        }
    }
    return 0;
}

tj_node *task_decl_at_cursor(const parse_slab *slab,
                              const TokenSpan *tokens, int num_tokens,
                              LspPos cursor) {
    for (tj_node *node = tj_node_at(slab, tokens, num_tokens, cursor);
         node != NULL; node = slab_node(slab, node->parent_node)) {
        if (node->keyword == KW_TASK &&
                pos_in_range(cursor, node->selection_range))
            return node;
    }
    return NULL;
}
