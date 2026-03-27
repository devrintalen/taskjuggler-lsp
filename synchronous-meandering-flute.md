# Plan: Hash Map for O(1) Dep-Ref Lookup in revalidate_dep_refs

## Context

Callgrind profiling of the flat 10k-task scenario showed that `resolve_from`
(diagnostics.c) + `strcmp` (libc, called by `resolve_from`) account for ~85%
of all instructions executed per session. The root cause is O(n²) behavior in
`revalidate_dep_refs`: for each of ~20k dep refs, `resolve_from` does a linear
scan through all n root-level task symbols comparing `detail` strings. For n=10k
and m=20k deps, that's ~200M strcmp calls. The fix is to build a hash map of the
root-level symbol array once before the dep-ref loop, reducing total work from
O(n×m) to O(n+m).

## File to modify

**`src/diagnostics.c`** only. The hash map is internal to this file — no header
changes are needed.

## Implementation

### 1. Add SymMap (open-addressing hash map, string → DocSymbol*)

Insert before `resolve_from`, around line 241:

```c
/* ── SymMap: string-keyed lookup table for DocSymbol pointers ────────────── *
 *
 * Open-addressing hash map with linear probing.  Keys are borrowed pointers
 * into DocSymbol.detail (owned by the ParseResult); the map does not copy or
 * free them.  Cap must be a power of two and at least 2× the entry count.
 */
typedef struct { const char *key; const DocSymbol *val; } SymMapEntry;
typedef struct { SymMapEntry *entries; int cap; } SymMap;

static void symmap_init(SymMap *m, int cap) {
    m->cap     = cap;
    m->entries = calloc((size_t)cap, sizeof(SymMapEntry));
}

static void symmap_free(SymMap *m) {
    free(m->entries);
    m->entries = NULL;
    m->cap     = 0;
}

/* FNV-1a hash */
static uint32_t symmap_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void symmap_insert(SymMap *m, const char *key, const DocSymbol *val) {
    uint32_t i = symmap_hash(key) & (uint32_t)(m->cap - 1);
    while (m->entries[i].key) {
        if (strcmp(m->entries[i].key, key) == 0) return;
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    m->entries[i] = (SymMapEntry){ key, val };
}

static const DocSymbol *symmap_get(const SymMap *m, const char *key) {
    uint32_t i = symmap_hash(key) & (uint32_t)(m->cap - 1);
    while (m->entries[i].key) {
        if (strcmp(m->entries[i].key, key) == 0) return m->entries[i].val;
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    return NULL;
}

/* Populate map from syms[], recursing into SK_MODULE transparently so that
 * tasks inside a project { } block are included at the same level as
 * top-level tasks. */
static void symmap_populate(SymMap *m, const DocSymbol *syms, int n) {
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION && syms[i].detail)
            symmap_insert(m, syms[i].detail, &syms[i]);
        else if (syms[i].kind == SK_MODULE)
            symmap_populate(m, syms[i].children, syms[i].num_children);
    }
}
```

### 2. Add resolve_from_map

Insert directly after `resolve_from`:

```c
/* Like resolve_from() but uses a pre-built SymMap for the first path segment.
 * Falls back to the regular linear resolve_from() for subsequent segments
 * (child arrays are small so linear scan is fine there). */
static const DocSymbol *resolve_from_map(const SymMap *map,
                                          const char **segs, int nseg) {
    if (nseg == 0) return NULL;
    const DocSymbol *first = symmap_get(map, segs[0]);
    if (!first) return NULL;
    if (nseg == 1) return first;
    return resolve_from(first->children, first->num_children, segs + 1, nseg - 1);
}
```

### 3. Rewrite revalidate_dep_refs to use the map

Replace the body of `revalidate_dep_refs` (keeping the trim/clear preamble
unchanged). The loop currently calls `validate_ref()` which calls `resolve_from()`
for every dep ref. Replace with inlined logic that uses the map for root-level
lookups:

```c
    /* Build root-level lookup map once for all dep refs in this document. */
    int map_cap = 16;
    while (map_cap < r->num_doc_symbols * 2 + 1) map_cap <<= 1;
    SymMap root_map;
    symmap_init(&root_map, map_cap);
    symmap_populate(&root_map, r->doc_symbols, r->num_doc_symbols);

    for (int i = 0; i < r->num_raw_dep_refs; i++) {
        const DepRef      *dr        = &r->raw_dep_refs[i];
        const DocSymbol   *sym       = NULL;
        const char        *found_uri = NULL;

        int k = dr->scope_n;

        /* Too many bangs: reference escapes the project root. */
        if (dr->bang_count > k) {
            push_diagnostic(r, dr->range, DIAG_WARNING,
                "dependency reference escapes beyond project root");
            continue;
        }

        /* nav_len: how many scope segments to navigate before searching.
         * nav_len == 0 means the search starts at the project root. */
        int nav_len = k - dr->bang_count;

        if (nav_len == 0) {
            /* Root-level lookup — use the hash map. */
            sym = resolve_from_map(&root_map,
                                   (const char **)dr->segs, dr->nseg);
        } else {
            /* Subtree lookup — navigate to the ancestor, then linear scan.
             * Child arrays are bounded by the tree branching factor so this
             * path stays fast. */
            int nctx;
            const DocSymbol *ctx = doc_symbol_find_path(
                r->doc_symbols, r->num_doc_symbols,
                (const char **)dr->scope, nav_len, &nctx);
            if (ctx)
                sym = resolve_from(ctx, nctx,
                                   (const char **)dr->segs, dr->nseg);
        }

        /* For absolute references (no bangs), also search other open files. */
        if (!sym && dr->bang_count == 0) {
            for (int p = 0; p < num_extra; p++) {
                sym = resolve_from(extra_pools[p], extra_counts[p],
                                   (const char **)dr->segs, dr->nseg);
                if (sym) { found_uri = extra_uris[p]; break; }
            }
        }

        if (sym) {
            push_def_link(r, dr->range, sym->selection_range, found_uri);
        } else {
            char path[256] = "";
            for (int j = 0; j < dr->nseg; j++) {
                if (j > 0) strncat(path, ".", sizeof(path) - strlen(path) - 1);
                strncat(path, dr->segs[j], sizeof(path) - strlen(path) - 1);
            }
            char msg[320];
            snprintf(msg, sizeof(msg), "unresolved task: `%s`", path);
            push_diagnostic(r, dr->range, DIAG_ERROR, msg);
        }
    }

    symmap_free(&root_map);
```

Note: `validate_ref` and the old `validate_dep_refs` function (which uses the
global g_dep_refs and appears to be dead code) are left unchanged.

## Why this is correct

- `symmap_populate` recurses into SK_MODULE (project containers) the same way
  `resolve_from` does, so tasks inside a `project { }` block are found correctly.
- `resolve_from_map` uses `resolve_from` for sub-segments, preserving the existing
  recursive traversal logic for dotted paths.
- The "too many bangs" diagnostic is emitted before the lookup and uses `continue`,
  matching the existing behavior where `validate_ref` returns NULL and the outer
  code emits the warning.
- Extra-pool search (cross-file absolute references) is unchanged.

## Verification

```bash
# 1. Build
make

# 2. Confirm flat-scenario timing drops from ~355ms to near 0ms for semantic-tokens:
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
  --requests semantic-tokens --positions 1 --repeat 2 --run ./taskjuggler-lsp

# 3. Full benchmark comparison (should see flat drop from 472ms to ~balanced range):
for label in flat balanced deep highdeps wide; do
  echo "=== $label ==="
  python3 tools/lsp_bench.py ./taskjuggler-lsp test/session_${label}.json \
    --iterations 5 --warmup 2
done

# 4. Smoke test against the tutorial file to confirm diagnostics still work:
python3 tools/lsp_perf_session.py test/tutorial.tjp \
  --requests semantic-tokens,document-symbol,hover,completion \
  --positions 5 --run ./taskjuggler-lsp
```

Expected outcome: flat scenario session total drops from ~470ms to the ~80-100ms
range (matching balanced/wide), driven by the first semantic-tokens response no
longer including 300+ms of dep-validation work.
