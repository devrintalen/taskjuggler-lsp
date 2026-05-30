/*
 * bench_snapshot.c — Microbenchmark isolating the two copy paths inside
 * workspace_snapshot_create():
 *
 *   Slab copy:   mmap(MAP_ANONYMOUS) + memcpy of the parse_slab page.
 *                This is what doc_snapshot_create() does — O(1) syscalls,
 *                bulk memory transfer.
 *
 *   Tree copy:   project_node_deep_copy() of the assembled ProjectNode tree.
 *                This is what workspace_snapshot_create() does for the
 *                primary project root — O(nodes) allocations + strdups.
 *
 * Usage:
 *   ./bench-snapshot <file.tjp> [--iterations N]
 *
 * Outputs timing statistics (min / mean / median / p95 / max) in
 * microseconds for each path across N timed iterations (default 500).
 */

#include "../src/parser.h"
#include "../src/project_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define DEFAULT_ITERATIONS 500

/* ── Timing ─────────────────────────────────────────────────────────────── */

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_stats(const char *label, double *samples, int n) {
    qsort(samples, (size_t)n, sizeof(double), cmp_double);
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += samples[i];
    double mean   = sum / n;
    double median = (n % 2 == 0)
        ? (samples[n/2 - 1] + samples[n/2]) / 2.0
        : samples[n/2];
    double p95    = samples[(int)(n * 0.95)];
    printf("  %-20s  min=%7.1f  mean=%7.1f  median=%7.1f  p95=%7.1f  max=%7.1f  (us)\n",
           label, samples[0], mean, median, p95, samples[n - 1]);
}

/* ── Node counting ──────────────────────────────────────────────────────── */

static int count_nodes(const ProjectNode *node) {
    if (!node) return 0;
    int total = 1;
    for (int i = 0; i < node->num_children; i++)
        total += count_nodes(node->children[i]);
    return total;
}

static int count_deps(const ProjectNode *node) {
    if (!node) return 0;
    int total = node->num_dependencies;
    for (int i = 0; i < node->num_children; i++)
        total += count_deps(node->children[i]);
    return total;
}

/* ── File I/O ───────────────────────────────────────────────────────────── */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf); return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return buf;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *tjp_path   = NULL;
    int         iterations = DEFAULT_ITERATIONS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
            if (iterations <= 0) { fprintf(stderr, "--iterations must be > 0\n"); return 1; }
        } else if (!tjp_path) {
            tjp_path = argv[i];
        } else {
            fprintf(stderr, "Usage: %s <file.tjp> [--iterations N]\n", argv[0]);
            return 1;
        }
    }
    if (!tjp_path) {
        fprintf(stderr, "Usage: %s <file.tjp> [--iterations N]\n", argv[0]);
        return 1;
    }

    /* ── Load and parse ─────────────────────────────────────────────────── */

    size_t file_len = 0;
    char *src = read_file(tjp_path, &file_len);
    if (!src) return 1;

    parse_slab *slab = parse(src);
    free(src);
    if (!slab) { fprintf(stderr, "parse() returned NULL\n"); return 1; }

    /* ── Build ProjectNode tree from the slab root ──────────────────────── */

    const char *uri = "file:///bench.tjp";
    ProjectNode *tree = project_node_from_tj(slab, slab->root_idx, uri);
    if (!tree) { fprintf(stderr, "project_node_from_tj() returned NULL\n"); return 1; }

    int node_count = count_nodes(tree);
    int dep_count  = count_deps(tree);
    size_t slab_bytes = slab->page ? slab->page->total_mmap_size : 0;

    printf("\n");
    printf("  Fixture:   %s\n", tjp_path);
    printf("  File size: %zu bytes\n", file_len);
    printf("  Slab size: %zu bytes\n", slab_bytes);
    printf("  Nodes:     %d\n", node_count);
    printf("  Deps:      %d\n", dep_count);
    printf("  Iters:     %d\n\n", iterations);

    double *slab_times = malloc((size_t)iterations * sizeof(double));
    double *tree_times = malloc((size_t)iterations * sizeof(double));
    if (!slab_times || !tree_times) { fprintf(stderr, "out of memory\n"); return 1; }

    /* ── Slab copy: mmap + memcpy + munmap ──────────────────────────────── */

    if (slab_bytes > 0) {
        for (int i = 0; i < iterations; i++) {
            double t0 = now_us();
            void *pg = mmap(NULL, slab_bytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (pg == MAP_FAILED) { perror("mmap"); return 1; }
            memcpy(pg, slab->page, slab_bytes);
            munmap(pg, slab_bytes);
            slab_times[i] = now_us() - t0;
        }
        print_stats("slab mmap+memcpy", slab_times, iterations);
    } else {
        printf("  slab mmap+memcpy: (no page — slab was malloc-backed)\n");
    }

    /* ── Tree copy: project_node_deep_copy ──────────────────────────────── */

    for (int i = 0; i < iterations; i++) {
        double t0 = now_us();
        ProjectNode *copy = project_node_deep_copy(tree);
        tree_times[i] = now_us() - t0;
        project_node_free(copy);
    }
    print_stats("tree deep_copy", tree_times, iterations);

    printf("\n");

    free(slab_times);
    free(tree_times);
    project_node_free(tree);
    parse_slab_free(slab);
    return 0;
}
