VERSION = 0.1.0

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_DEFAULT_SOURCE
LDFLAGS = -lcjson

# Generated files from flex and bison
GEN_LEX  = src/lexer.yy.c
GEN_GRAM = src/grammar.tab.c
GEN_HDR  = src/grammar.tab.h

SRC = src/main.c src/server.c src/parser.c src/diagnostics.c \
      $(GEN_LEX) $(GEN_GRAM) \
      src/document_symbol.c src/folding_range.c src/hover.c src/signature.c src/completion.c src/semantic_tokens.c src/definition.c src/references.c

OBJ = $(SRC:.c=.o)
BIN = taskjuggler-lsp

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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

# Links only the generated lexer — no parser, server, or cJSON needed.
$(LEXTEST_BIN): $(GEN_HDR) $(GEN_LEX) $(LEXTEST_SRC)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(LEXTEST_SRC) $(GEN_LEX)

clean:
	rm -f $(OBJ) $(BIN) $(GEN_LEX) $(GEN_GRAM) $(GEN_HDR)
	rm -f $(LEXTEST_BIN) tools/lexer_test.o

.PHONY: all clean lexer-test
