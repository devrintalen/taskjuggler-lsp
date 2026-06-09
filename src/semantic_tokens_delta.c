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

#include "semantic_tokens_delta.h"
#include "semantic_tokens.h"
#include "rpc.h"

#include <stdlib.h>
#include <string.h>

/** Number of uint32 entries per semantic token in the LSP data array.
 *  Diffing operates on these five-tuples as atomic elements, which keeps
 *  every emitted edit aligned to a token boundary. */
#define TUPLE_LEN 5

/** Maximum edit distance the Myers diff will compute before falling back
 *  to a single replace-everything edit.  Sized so worst-case snapshot
 *  storage stays under a few MB; realistic deltas (a typing burst inside
 *  a kilo-line file) reduce to D < 100 after prefix/suffix trimming. */
#define D_BOUND 1024

/** Single coalesced edit operation expressed in token units. */
typedef struct {
    size_t start_tok;        /**< offset in the previous data buffer (tokens) */
    size_t delete_tok;       /**< number of tokens to remove */
    size_t insert_b_start;   /**< offset in the new data buffer (tokens) */
    size_t insert_tok;       /**< number of tokens to insert from there */
} EditOp;

/**
 * Compare two five-tuples for byte equality.
 *
 * @param a  Pointer to the first five-element uint32_t tuple.
 * @param b  Pointer to the second five-element uint32_t tuple.
 * @return   Non-zero if the tuples are identical, zero otherwise.
 */
static int tokens_equal(const uint32_t *a, const uint32_t *b) {
    return memcmp(a, b, TUPLE_LEN * sizeof(uint32_t)) == 0;
}

/* ── Myers O(ND) diff over five-tuple sequences ─────────────────────────── *
 *
 * Implements the original Myers algorithm (Eugene W. Myers, 1986, "An
 * O(ND) Difference Algorithm and Its Variations") operating on
 * five-uint32 elements compared by memcmp.  V snapshots are stored
 * compactly: snapshot d occupies d+1 entries starting at offset
 * d*(d+1)/2.
 *
 * On termination at edit distance @p d_found, snapshots 0..d_found-1
 * are populated and used to backtrace the optimal edit script.  The
 * raw script is then coalesced — runs of adjacent insertions and
 * deletions at the same a-position become a single EditOp.
 */

/**
 * Compute the minimal edit script from sequence @p a (length @p na
 * tokens) to sequence @p b (length @p nb tokens) using Myers' diff,
 * then coalesce adjacent operations into LSP-style edits.
 *
 * Token offsets in the output are shifted by @p a_offset and
 * @p b_offset so the caller can position the edits inside larger
 * surrounding buffers (after prefix/suffix trimming).
 *
 * When @p na + @p nb exceeds #D_BOUND the algorithm falls back to a
 * single replace-everything edit to bound snapshot memory.
 *
 * @param a          Pointer to the previous (old) token data array.
 * @param na         Number of tokens in @p a.
 * @param b          Pointer to the new token data array.
 * @param nb         Number of tokens in @p b.
 * @param a_offset   Token index offset added to each edit's start position
 *                   (accounts for already-trimmed common prefix in @p a).
 * @param b_offset   Token index offset added to each edit's insert position
 *                   (accounts for already-trimmed common prefix in @p b).
 * @param out_ops    Output pointer set to a heap-allocated array of coalesced
 *                   #EditOp values; caller must free.  Set to NULL when there
 *                   are no edits.
 * @param out_n_ops  Output pointer set to the number of entries in
 *                   @p out_ops.
 */
/**
 * Coalesce a raw edit-step list into grouped EditOps.
 *
 * @p raw_kinds holds per-step kinds (0 = snake/match, 1 = delete from a,
 * 2 = insert from b) in reverse chronological order. This walks the script
 * chronologically (reverse of recorded order): a snake advances the running
 * position in both sequences and closes any open edit run, while consecutive
 * delete/insert steps merge into one EditOp spanning that run.
 *
 * @param raw_kinds  Step kinds, recorded in reverse chronological order.
 * @param n_raw      Number of steps in @p raw_kinds.
 * @param a_offset   Token offset of the diff window within the old buffer.
 * @param b_offset   Token offset of the diff window within the new buffer.
 * @param max_ops    Upper bound on edit runs (the Myers edit distance).
 * @param out_ops    Receives the malloc'd EditOp array (caller frees).
 * @param out_n_ops  Receives the number of EditOps produced.
 */
static void coalesce_edit_ops(const int *raw_kinds, size_t n_raw,
                              size_t a_offset, size_t b_offset, size_t max_ops,
                              EditOp **out_ops, size_t *out_n_ops) {
    EditOp *ops = malloc(max_ops * sizeof(EditOp));
    size_t n_ops = 0;
    size_t cur_x = 0, cur_y = 0;
    size_t idx = n_raw;
    while (idx > 0) {
        if (raw_kinds[idx - 1] == 0) {
            cur_x++;
            cur_y++;
            idx--;
            continue;
        }
        size_t edit_start_x = cur_x;
        size_t edit_start_b = cur_y;
        size_t del = 0, ins = 0;
        while (idx > 0 && raw_kinds[idx - 1] != 0) {
            if (raw_kinds[idx - 1] == 1) {
                cur_x++;
                del++;
            } else {
                cur_y++;
                ins++;
            }
            idx--;
        }
        ops[n_ops].start_tok      = a_offset + edit_start_x;
        ops[n_ops].delete_tok     = del;
        ops[n_ops].insert_b_start = b_offset + edit_start_b;
        ops[n_ops].insert_tok     = ins;
        n_ops++;
    }
    *out_ops   = ops;
    *out_n_ops = n_ops;
}

static void myers_diff_run(const uint32_t *a, size_t na,
                           const uint32_t *b, size_t nb,
                           size_t a_offset, size_t b_offset,
                           EditOp **out_ops, size_t *out_n_ops) {
    /* Trivial: nothing on either side. */
    if (na == 0 && nb == 0) {
        *out_ops   = NULL;
        *out_n_ops = 0;
        return;
    }
    /* Trivial: one side empty, or input too large for Myers — emit one
     * replace edit covering the whole middle. */
    if (na == 0 || nb == 0 || na + nb > D_BOUND) {
        EditOp *op = malloc(sizeof(EditOp));
        op->start_tok      = a_offset;
        op->delete_tok     = na;
        op->insert_b_start = b_offset;
        op->insert_tok     = nb;
        *out_ops   = op;
        *out_n_ops = 1;
        return;
    }

    int max_total_i = (int)(na + nb);
    size_t v_len = (size_t)(2 * max_total_i + 1);
    int *v = calloc(v_len, sizeof(int));

    /* Snapshot storage: snapshot d has d+1 entries starting at d*(d+1)/2.
     * Total = max_total * (max_total + 1) / 2 (snapshots 0..max_total-1). */
    size_t snap_total = (size_t)max_total_i * (size_t)(max_total_i + 1) / 2;
    int *snap = calloc(snap_total, sizeof(int));

    int d_found = -1;
    int x_final = 0, y_final = 0;

    for (int d = 0; d <= max_total_i; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[k - 1 + max_total_i] < v[k + 1 + max_total_i])) {
                /* Move down: this step is an insertion from b. */
                x = v[k + 1 + max_total_i];
            } else {
                /* Move right: this step is a deletion from a. */
                x = v[k - 1 + max_total_i] + 1;
            }
            int y = x - k;
            /* Slide along any matching diagonal "snake". */
            while (x < (int)na && y < (int)nb &&
                   tokens_equal(&a[(size_t)x * TUPLE_LEN], &b[(size_t)y * TUPLE_LEN])) {
                x++;
                y++;
            }
            v[k + max_total_i] = x;
            if (x >= (int)na && y >= (int)nb) {
                d_found = d;
                x_final = x;
                y_final = y;
                goto found;
            }
        }
        /* Snapshot V at the end of this d so backtrace can read V[d-1]. */
        size_t off = (size_t)d * (size_t)(d + 1) / 2;
        for (int i = 0; i <= d; i++) {
            int kk = -d + 2 * i;
            snap[off + i] = v[kk + max_total_i];
        }
    }

found:
    free(v);

    /* d_found == 0 means the entire middle was a single snake — no edits.
     * d_found < 0 is unreachable (the loop terminates by d = max_total_i
     * at the latest) but guarding here lets the compiler narrow the range
     * for the ops allocation below. */
    if (d_found <= 0) {
        free(snap);
        *out_ops   = NULL;
        *out_n_ops = 0;
        return;
    }

    /* Backtrace: walk from (x_final, y_final) at d_found down to (0, 0),
     * recording each step as a snake/delete/insert.  Steps are recorded
     * in reverse chronological order. */
    int   *raw_kinds = malloc((na + nb) * sizeof(int));
    size_t n_raw     = 0;

    int x = x_final, y = y_final;
    for (int d = d_found; d > 0; d--) {
        int k = x - y;
        int down;
        if (k == -d) {
            down = 1;
        } else if (k == d) {
            down = 0;
        } else {
            size_t off = (size_t)(d - 1) * (size_t)d / 2;
            int idx_km1 = (k - 1 + (d - 1)) / 2;
            int idx_kp1 = (k + 1 + (d - 1)) / 2;
            int vkm1 = snap[off + idx_km1];
            int vkp1 = snap[off + idx_kp1];
            down = (vkm1 < vkp1) ? 1 : 0;
        }
        int k_prev = down ? k + 1 : k - 1;
        size_t off_prev   = (size_t)(d - 1) * (size_t)d / 2;
        int    idx_prev   = (k_prev + (d - 1)) / 2;
        int    x_prev     = snap[off_prev + idx_prev];
        int    y_prev     = x_prev - k_prev;
        int    x_edge     = down ? x_prev : x_prev + 1;
        int    y_edge     = down ? y_prev + 1 : y_prev;

        /* Snake from (x, y) back to (x_edge, y_edge). */
        while (x > x_edge && y > y_edge) {
            x--; y--;
            raw_kinds[n_raw++] = 0;
        }
        /* Single edit step from (x_edge, y_edge) back to (x_prev, y_prev). */
        if (down) {
            y--;
            raw_kinds[n_raw++] = 2;
        } else {
            x--;
            raw_kinds[n_raw++] = 1;
        }
    }
    /* Leading snake from (0, 0) up to wherever the d=0 path landed. */
    while (x > 0 && y > 0) {
        x--; y--;
        raw_kinds[n_raw++] = 0;
    }

    free(snap);

    /* Group the raw step list into one EditOp per run of edits. */
    coalesce_edit_ops(raw_kinds, n_raw, a_offset, b_offset,
                      (size_t)d_found, out_ops, out_n_ops);
    free(raw_kinds);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/** Serialize one diff EditOp to an LSP SemanticTokensEdit object: the start
 *  offset and delete count (converted from token units to flat-integer
 *  units), plus the inserted token data when the op adds tokens.
 *  @param doc      Destination mutable JSON document.
 *  @param op       Edit operation to serialize.
 *  @param new_buf  New token buffer the op's inserted slice points into.
 *  @return The SemanticTokensEdit object. */
static yyjson_mut_val *build_edit_json(yyjson_mut_doc *doc, const EditOp *op,
                                       const uint32_t *new_buf) {
    yyjson_mut_val *edit = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, edit, "start",
                            (uint64_t)(op->start_tok * TUPLE_LEN));
    yyjson_mut_obj_add_uint(doc, edit, "deleteCount",
                            (uint64_t)(op->delete_tok * TUPLE_LEN));
    if (op->insert_tok > 0) {
        yyjson_mut_val *data = build_uint32_array_json(
            doc,
            &new_buf[op->insert_b_start * TUPLE_LEN],
            op->insert_tok * TUPLE_LEN);
        yyjson_mut_obj_add_val(doc, edit, "data", data);
    }
    return edit;
}

yyjson_mut_val *build_semantic_tokens_delta_json(yyjson_mut_doc *doc,
                                                  const uint32_t *prev_buf, size_t prev_count,
                                                  const uint32_t *new_buf,  size_t new_count,
                                                  const char *result_id) {
    size_t prev_tok = prev_count / TUPLE_LEN;
    size_t new_tok  = new_count  / TUPLE_LEN;

    /* Trim common prefix in token units. */
    size_t prefix = 0;
    size_t min_tok = prev_tok < new_tok ? prev_tok : new_tok;
    while (prefix < min_tok &&
           tokens_equal(&prev_buf[prefix * TUPLE_LEN],
                        &new_buf[prefix * TUPLE_LEN])) {
        prefix++;
    }

    /* Trim common suffix in token units (must not overlap prefix). */
    size_t suffix = 0;
    while (suffix < (prev_tok - prefix) && suffix < (new_tok - prefix) &&
           tokens_equal(&prev_buf[(prev_tok - 1 - suffix) * TUPLE_LEN],
                        &new_buf [(new_tok  - 1 - suffix) * TUPLE_LEN])) {
        suffix++;
    }

    const uint32_t *a = prev_buf + prefix * TUPLE_LEN;
    size_t a_len = prev_tok - prefix - suffix;
    const uint32_t *b = new_buf + prefix * TUPLE_LEN;
    size_t b_len = new_tok - prefix - suffix;

    EditOp *ops = NULL;
    size_t  n_ops = 0;
    myers_diff_run(a, a_len, b, b_len, prefix, prefix, &ops, &n_ops);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    if (result_id)
        yyjson_mut_obj_add_strcpy(doc, result, "resultId", result_id);

    yyjson_mut_val *edits = yyjson_mut_arr(doc);
    for (size_t i = 0; i < n_ops; i++)
        yyjson_mut_arr_add_val(edits, build_edit_json(doc, &ops[i], new_buf));
    free(ops);
    yyjson_mut_obj_add_val(doc, result, "edits", edits);
    return result;
}

yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                  yyjson_val *params,
                                                  const query_context *qc,
                                                  const query_doc *d) {
    const char *previous_result_id = params ? json_str(params, "previousResultId") : NULL;
    if (!d || !d->snap || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    const uint32_t *new_buf = NULL;
    size_t          new_count = 0;
    docsnap_sem_tokens(d->snap, &new_buf, &new_count);

    char result_id[32];
    sem_tokens_result_id(d->snap, result_id, sizeof(result_id));

    /* Identify the snapshot the client is holding by its resultId (== that
     * snapshot's parse version): either the current parse (no change since)
     * or the immediately previous one retained on the query_context.  Diff
     * against that base; otherwise fall back to a full response. */
    doc_snapshot *base = NULL;
    if (previous_result_id) {
        uint64_t prev_version = strtoull(previous_result_id, NULL, 10);
        if (prev_version == d->snap->doc_version)
            base = d->snap;
        else if (qc->prev_snap && prev_version == qc->prev_snap->doc_version)
            base = qc->prev_snap;
    }

    yyjson_mut_val *result;
    if (base) {
        const uint32_t *old_buf = NULL;
        size_t          old_count = 0;
        docsnap_sem_tokens(base, &old_buf, &old_count);
        result = build_semantic_tokens_delta_json(doc, old_buf, old_count,
                                                   new_buf, new_count, result_id);
    } else {
        result = build_semantic_tokens_json_from_buf(doc, new_buf, new_count, result_id);
    }
    return make_response(doc, id, result);
}
