# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A Language Server Protocol (LSP) implementation for [TaskJuggler](https://taskjuggler.org/), written in C.

The project supports TaskJuggler v3 and does not support earlier versions.

## Dependencies

Depends on:
- cJSON
- Flex
- Bison

## Architecture

To be documented as the codebase develops. Expected structure:
- LSP server handling JSON-RPC communication
- TaskJuggler `.tjp`/`.tji` file parser
- Language features: diagnostics, completion, hover, go-to-definition

## LSP Feature Status

| Feature | Method | Status | Notes |
|---|---|---|---|
| Lifecycle | `initialize` / `shutdown` / `exit` | Implemented | Negotiates capabilities on init |
| Document Sync | `textDocument/didOpen`, `didChange`, `didClose` | Implemented | Full-document sync; caches up to 64 open files |
| Diagnostics | `textDocument/publishDiagnostics` | Implemented | Reports unresolved `depends`/`precedes` targets as errors; out-of-scope relative refs as warnings |
| Hover | `textDocument/hover` | Implemented | Markdown docs for 40+ TaskJuggler keywords |
| Completion | `textDocument/completion` | Implemented | Context-aware keyword and identifier suggestions; supports hierarchical and relative (`!`) references |
| Signature Help | `textDocument/signatureHelp` | Implemented | Parameter descriptions for 35+ keywords |
| Document Symbols | `textDocument/documentSymbol` | Implemented | Hierarchical symbol tree for projects, tasks, resources, accounts, shifts |
| Go to Definition | `textDocument/definition` | Not implemented | |
| Find References | `textDocument/references` | Not implemented | |
| Rename | `textDocument/rename` | Not implemented | |
| Code Actions | `textDocument/codeAction` | Not implemented | |
| Formatting | `textDocument/formatting` | Not implemented | |
| Folding Ranges | `textDocument/foldingRange` | Not implemented | |
| Workspace Symbols | `workspace/symbol` | Not implemented | |


## Workflow

When implementing new functionality, do not jump right to committing
and pushing with Git. Wait until I ask to do so. I will test new
functions before doing so.
