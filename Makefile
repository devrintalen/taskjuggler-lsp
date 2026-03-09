CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_DEFAULT_SOURCE
LDFLAGS = -lcjson

SRC = src/main.c src/server.c src/parser.c \
      src/hover.c src/signature.c src/completion.c \
      src/semantic_tokens.c
OBJ = $(SRC:.c=.o)
BIN = taskjuggler-lsp

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
