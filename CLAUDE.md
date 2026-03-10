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


## Workflow

When implementing new functionality, do not jump right to committing
and pushing with Git. Wait until I ask to do so. I will test new
functions before doing so.
