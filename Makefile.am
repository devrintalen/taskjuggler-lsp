# Non-recursive automake build.  `autoreconf -i && ./configure && make`
# replaces the old hand-written Makefile; the target names it provided
# (debug, lexer-test, parse-bench, docs, docs-clean) are preserved below.

# CFLAGS_EXTRA is for one-off overrides, e.g. enabling per-category debug
# logging without editing src/debug.h:
#   make CFLAGS_EXTRA="-DDEBUG_TJ3=3 -DDEBUG_REVALIDATE=2"
CFLAGS_EXTRA =

# The build directory is on the include path first so that translation units
# in $(srcdir) find the generated src/grammar.tab.h in out-of-tree builds.
AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_srcdir)/src -D_DEFAULT_SOURCE \
              $(CFLAGS_EXTRA)

WARN_CFLAGS = -Wall -Wextra

# -flto=auto: the per-token hot path crosses translation units — the flex
# scanner (lexer.yy.c) calls g_push_tok_span / arena_strndup / emit_* in
# parser.c on every one of the hundreds of thousands of tokens a large parse
# produces.  Link-time optimization inlines those across TUs, which measured
# ~3-5% off the parse on every fixture.  The debug build deliberately omits
# it so perf / valgrind still see real function boundaries.
AM_CFLAGS = $(WARN_CFLAGS) -std=c11 -flto=auto

DEBUG_CFLAGS = $(WARN_CFLAGS) -std=c11 -O2 -g

bin_PROGRAMS = taskjuggler-lsp
EXTRA_PROGRAMS = taskjuggler-lsp-debug lexer-test parse-bench

# ── Generated lexer / parser ─────────────────────────────────────────────── #

# Bison must run before flex so that grammar.tab.h exists when lexer.l is
# compiled (the flex output #includes grammar.tab.h for token codes).
# BUILT_SOURCES guarantees the header exists before any object is compiled.
GEN_LEX  = src/lexer.yy.c
GEN_GRAM = src/grammar.tab.c
GEN_HDR  = src/grammar.tab.h

BUILT_SOURCES = $(GEN_HDR) $(GEN_GRAM) $(GEN_LEX)
CLEANFILES = $(GEN_HDR) $(GEN_GRAM) $(GEN_LEX)

$(GEN_GRAM): src/grammar.y
	$(MKDIR_P) src
	$(BISON) -d -o $@ $(top_srcdir)/src/grammar.y

# grammar.tab.h is produced by the same bison run; this recover rule keeps
# parallel makes from running bison twice for the two outputs.
$(GEN_HDR): $(GEN_GRAM)
	@if test ! -f $@; then rm -f $(GEN_GRAM); \
	    $(MAKE) $(AM_MAKEFLAGS) $(GEN_GRAM); else :; fi

$(GEN_LEX): src/lexer.l $(GEN_HDR)
	$(MKDIR_P) src
	$(FLEX) -o $@ $(top_srcdir)/src/lexer.l

# The generated files live in convenience libraries so -Wno-unused-function
# (for warnings we cannot fix upstream) applies to them alone, not to the
# hand-written sources.  The debug variant recompiles them with DEBUG_CFLAGS
# so profilers see symbols in the lexer/parser hot path.
noinst_LIBRARIES = libtjlsp_gen.a
EXTRA_LIBRARIES = libtjlsp_gen_debug.a

nodist_libtjlsp_gen_a_SOURCES = $(GEN_LEX) $(GEN_GRAM)
libtjlsp_gen_a_CFLAGS = $(AM_CFLAGS) -Wno-unused-function

nodist_libtjlsp_gen_debug_a_SOURCES = $(GEN_LEX) $(GEN_GRAM)
libtjlsp_gen_debug_a_CFLAGS = $(DEBUG_CFLAGS) -Wno-unused-function

# ── Server ───────────────────────────────────────────────────────────────── #

server_sources = \
    src/main.c \
    src/server.c src/server.h \
    src/parser.c src/parser.h \
    src/diagnostics.c src/diagnostics.h \
    src/debug.c src/debug.h \
    src/arena.c src/arena.h \
    src/job_queue.c src/job_queue.h \
    src/threadpool.c src/threadpool.h \
    src/compile_commands.c src/compile_commands.h \
    src/rpc.c src/rpc.h \
    src/pathutil.c src/pathutil.h \
    src/document_symbol.c src/document_symbol.h \
    src/folding_range.c src/folding_range.h \
    src/hover.c src/hover.h \
    src/signature.c src/signature.h \
    src/completion.c src/completion.h \
    src/semantic_tokens.c src/semantic_tokens.h \
    src/semantic_tokens_delta.c src/semantic_tokens_delta.h \
    src/dependency.c src/dependency.h \
    src/definition.c src/definition.h \
    src/references.c src/references.h \
    src/document_highlight.c src/document_highlight.h \
    src/workspace_symbol.c src/workspace_symbol.h \
    src/code_lens.c src/code_lens.h \
    src/project_tree.c src/project_tree.h \
    src/query_context.c src/query_context.h \
    src/tj3.c src/tj3.h \
    src/diag_worker.c src/diag_worker.h \
    src/version.h

taskjuggler_lsp_SOURCES = $(server_sources)
taskjuggler_lsp_LDADD = libtjlsp_gen.a

# Debug build with symbols for use with perf/valgrind (requires -no-pie for
# callgrind to resolve symbols).  Build with `make debug`.
taskjuggler_lsp_debug_SOURCES = $(server_sources)
taskjuggler_lsp_debug_CFLAGS = $(DEBUG_CFLAGS)
taskjuggler_lsp_debug_LDFLAGS = -no-pie
taskjuggler_lsp_debug_LDADD = libtjlsp_gen_debug.a

debug: taskjuggler-lsp-debug$(EXEEXT)

# ── Standalone lexer test ────────────────────────────────────────────────── #

# Links only the generated lexer — no parser, server, or JSON library needed.
# Build with `make lexer-test`.
lexer_test_SOURCES = tools/lexer_test.c
nodist_lexer_test_SOURCES = $(GEN_LEX)
lexer_test_CFLAGS = $(AM_CFLAGS) -Wno-unused-function

# ── Standalone parse() microbenchmark ────────────────────────────────────── #

# Times parse() in isolation (no server, threads, or tj3 worker), so the
# numbers are not perturbed by the background diagnostics worker the way the
# round-trip tools/bench_didchange.py timings are.  Usage:
#   make parse-bench && ./parse-bench test/perf_highdeps.tjp 25
#
# parse() and the symbols it transitively needs; diagnostics.c references
# lsp_send_message, which the harness stubs out (the parse path never calls
# it).  The src/*.c objects are shared with the main server build.
parse_bench_SOURCES = \
    tools/parse_bench.c \
    src/parser.c \
    src/arena.c \
    src/debug.c \
    src/diagnostics.c \
    src/pathutil.c
parse_bench_LDADD = libtjlsp_gen.a

# ── Test suite ───────────────────────────────────────────────────────────── #

# Golden-file harness; `make check` is equivalent to
#   python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases
check-local:
	$(PYTHON) $(top_srcdir)/tools/lsp_test.py \
	    ./taskjuggler-lsp$(EXEEXT) --all $(top_srcdir)/test/cases

# ── Documentation ────────────────────────────────────────────────────────── #

docs:
	cd $(top_srcdir) && $(DOXYGEN) Doxyfile

docs-clean:
	rm -rf $(top_srcdir)/doc/_doxygen/

# ── Distribution ─────────────────────────────────────────────────────────── #

EXTRA_DIST = \
    src/grammar.y \
    src/lexer.l \
    tools \
    test \
    doc \
    Doxyfile \
    README.md \
    CLAUDE.md

clean-local:
	rm -f taskjuggler-lsp-debug$(EXEEXT) lexer-test$(EXEEXT) parse-bench$(EXEEXT)

.PHONY: debug docs docs-clean
