# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when
working with code in this repository.

## Project

A Language Server Protocol (LSP) implementation for
[TaskJuggler](https://taskjuggler.org/), written in C.

The project supports TaskJuggler v3 and does not support earlier
versions.

## Dependencies

Depends on:
- yyjson
- Flex
- Bison

## Architecture

To be documented as the codebase develops. Expected structure:
- LSP server handling JSON-RPC communication
- TaskJuggler `.tjp`/`.tji` file parser
- Language features: diagnostics, completion, hover, go-to-definition

## Code Style Conventions

Use snake_case rather than camelCase for multi-word identifiers.

Use K&R C style for code.

Use spaces instead of tabs, and use four spaces per indent.

Prefer full words rather than abbreviations for naming. For example,
"token_end_line" rather than "tok_el".

## TaskJuggler Reference

Use the command "tj3man" to get definitive syntax and usage
information for TaskJuggler. The command "tj3man <keyword>" will
return detailed information on keywords and concepts. "tj3man" without
any arguments will return a full list of available pages.

## Release Checklist

When cutting a new release, perform every step below:

1. Bump `VERSION_MAJOR` / `VERSION_MINOR` / `VERSION_PATCH` and
   `VERSION_STRING` in `src/version.h`.
2. Bump `VERSION` in the `Makefile` to match.
3. Rebuild (`make`) and run the full test suite:
   `python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases`.
   The `initialize` response embeds `VERSION_STRING` in `serverInfo`,
   so every `expected.json` that captures an initialize reply must be
   updated to the new version. Use `--record` to regenerate them, or
   update the `"version": "X.Y.Z"` string in place.
4. Commit the version bump together with the updated test snapshots.

