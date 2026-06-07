VERSION = 0.5.3

CC      = gcc
# CFLAGS_EXTRA is for one-off overrides, e.g. enabling per-category debug
# logging without editing src/debug.h:
#   make CFLAGS_EXTRA="-DDEBUG_TJ3=3 -DDEBUG_REVALIDATE=2"
CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_DEFAULT_SOURCE -MMD -MP $(CFLAGS_EXTRA)
LDFLAGS = -lyyjson -lpthread

# Generated files from flex and bison
GEN_LEX  = src/lexer.yy.c
GEN_GRAM = src/grammar.tab.c
GEN_HDR  = src/grammar.tab.h

SRC = src/main.c src/server.c src/parser.c src/diagnostics.c src/debug.c \
      src/job_queue.c src/threadpool.c src/compile_commands.c \
      src/rpc.c src/pathutil.c \
      $(GEN_LEX) $(GEN_GRAM) \
      src/document_symbol.c src/folding_range.c src/hover.c src/signature.c src/completion.c src/semantic_tokens.c src/semantic_tokens_delta.c src/dependency.c src/definition.c src/references.c src/document_highlight.c src/workspace_symbol.c src/code_lens.c src/project_tree.c src/query_context.c src/tj3.c src/diag_worker.c

OBJ = $(SRC:.c=.o)
BIN = taskjuggler-lsp

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Debug build with symbols for use with perf/valgrind.
# Output binary: taskjuggler-lsp-debug
DEBUG_CFLAGS = -Wall -Wextra -std=c11 -O2 -g -no-pie -D_DEFAULT_SOURCE -MMD -MP $(CFLAGS_EXTRA)
DEBUG_OBJ    = $(SRC:.c=.debug.o)
DEBUG_BIN    = taskjuggler-lsp-debug

debug: $(DEBUG_BIN)

$(DEBUG_BIN): $(DEBUG_OBJ)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS)

$(DEBUG_OBJ): $(GEN_HDR)

src/lexer.yy.debug.o: src/lexer.yy.c
	$(CC) $(DEBUG_CFLAGS) -Wno-unused-function -c -o $@ $<

src/grammar.tab.debug.o: src/grammar.tab.c
	$(CC) $(DEBUG_CFLAGS) -Wno-unused-function -c -o $@ $<

%.debug.o: %.c
	$(CC) $(DEBUG_CFLAGS) -c -o $@  $<

# All object files need the generated header for token type definitions.
$(OBJ): $(GEN_HDR)

# Bison must run before flex so that grammar.tab.h exists when lexer.l is
# compiled (the flex output #includes grammar.tab.h for token codes).
$(GEN_GRAM) $(GEN_HDR): src/grammar.y
	bison -d -o $(GEN_GRAM) $<

$(GEN_LEX): src/lexer.l $(GEN_HDR)
	flex -o $@ $<

# Suppress warnings in generated files that we cannot fix upstream.
$(GEN_LEX:.c=.o): $(GEN_LEX)
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

$(GEN_GRAM:.c=.o): $(GEN_GRAM)
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Standalone lexer test ────────────────────────────────────────────────── #

LEXTEST_BIN = lexer-test
LEXTEST_SRC = tools/lexer_test.c

# Links only the generated lexer — no parser, server, or JSON library needed.
$(LEXTEST_BIN): $(GEN_HDR) $(GEN_LEX) $(LEXTEST_SRC)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(LEXTEST_SRC) $(GEN_LEX)

clean:
	rm -f $(OBJ) $(BIN) $(GEN_LEX) $(GEN_GRAM) $(GEN_HDR)
	rm -f $(DEBUG_OBJ) $(DEBUG_BIN)
	rm -f $(LEXTEST_BIN) tools/lexer_test.o
	rm -f $(OBJ:.o=.d) $(DEBUG_OBJ:.o=.d)

# Auto-generated header dependencies from -MMD.
-include $(OBJ:.o=.d)
-include $(DEBUG_OBJ:.o=.d)

# ── Documentation ────────────────────────────────────────────────────────── #

docs:
	doxygen Doxyfile

docs-clean:
	rm -rf doc/_doxygen/

.PHONY: all debug clean lexer-test docs docs-clean
