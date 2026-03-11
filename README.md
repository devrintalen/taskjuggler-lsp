# taskjuggler-lsp

A [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) (LSP) server for [TaskJuggler v3](https://taskjuggler.org/), written in C.

This is an independent implementation that provides editor tooling for TaskJuggler's `.tjp`/`.tji` file format. It is not a modified version of TaskJuggler and does not include any TaskJuggler source code.

## Features

| Feature | Status |
|---|---|
| Diagnostics | Implemented |
| Hover documentation | Implemented |
| Completion | Implemented |
| Signature help | Implemented |
| Document symbols | Implemented |
| Go to definition | Not implemented |
| Find references | Not implemented |

## Dependencies

- [cJSON](https://github.com/DaveGamble/cJSON)
- [Flex](https://github.com/westes/flex)
- [Bison](https://www.gnu.org/software/bison/)

On Debian/Ubuntu:

```sh
apt install libcjson-dev flex bison
```

## Building

```sh
make
```

This produces the `taskjuggler-lsp` binary. To also build the standalone lexer test tool:

```sh
make lexer-test
```

To clean build artifacts:

```sh
make clean
```

## Usage

Configure your editor to launch `taskjuggler-lsp` as the language server for `.tjp` and `.tji` files. The server communicates over standard input/output using the LSP JSON-RPC protocol.

## License

Copyright (C) 2026 Devrin Talen

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version. See [LICENSE](LICENSE) for the full license text.

### Relationship to TaskJuggler

[TaskJuggler](https://taskjuggler.org/) is copyright Chris Schlaeger and others, licensed under GPLv2. This project is a separate tool that supports the TaskJuggler file format; it is not a derivative work of the TaskJuggler source code.

The file `test/tutorial.tjp` is an example project from the TaskJuggler tutorial, copyright Chris Schlaeger, included here as a test fixture under the terms of the GPLv2.
