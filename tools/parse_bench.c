/* Standalone parse() microbenchmark — isolates the parse path from the
 * server, threads, and the tj3 background worker so timings are not perturbed
 * by anything but lexing/parsing itself (the round-trip tools/bench_didchange.py
 * numbers are noisy because the diag worker competes for the CPU/allocator).
 *
 * Build and run via the Makefile:
 *   make parse-bench && ./parse-bench test/perf_highdeps.tjp 25
 */
#include "../src/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Stub for the one server symbol diagnostics.o references but the parse path
 * never reaches (publish_diagnostics_list).  Lets us link without server.o. */
void lsp_send_message(const char *msg);
void lsp_send_message(const char *msg) { (void)msg; }

static double ms_since(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [rounds]\n", argv[0]); return 2; }
    int rounds = argc > 2 ? atoi(argv[2]) : 20;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n + 1);
    if (fread(src, 1, (size_t)n, f) != (size_t)n) { perror("fread"); return 1; }
    src[n] = '\0';
    fclose(f);

    double best = 1e18, sum = 0;
    int toks = 0;
    double *all = malloc((size_t)rounds * sizeof(double));
    for (int i = 0; i < rounds; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ParseOutput *po = parse(src);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = ms_since(t0, t1);
        toks = po->num_tok_spans;
        parse_output_free(po);
        all[i] = dt;
        if (i > 0) { if (dt < best) best = dt; sum += dt; }  /* drop warmup */
    }
    /* median of the post-warmup samples */
    for (int i = 1; i < rounds; i++)
        for (int j = i + 1; j < rounds; j++)
            if (all[j] < all[i]) { double t = all[i]; all[i] = all[j]; all[j] = t; }
    double median = all[1 + (rounds - 1) / 2];
    printf("%-24s tokens=%d  min=%.1f  median=%.1f  mean=%.1f ms (n=%d)\n",
           argv[1], toks, best, median, sum / (rounds - 1), rounds - 1);
    free(all); free(src);
    return 0;
}
