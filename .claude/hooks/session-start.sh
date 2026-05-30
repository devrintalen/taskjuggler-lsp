#!/bin/bash
#
# SessionStart hook for Claude Code on the web.
#
# Installs the build/runtime dependencies for taskjuggler-lsp so that
# `make` and the test suite work in a fresh remote container. Mirrors the
# package set in .devcontainer/Dockerfile:
#   - build-essential: gcc + make
#   - flex / bison:    lexer and grammar generation
#   - cmake:           used to build yyjson from source
#   - valgrind:        callgrind profiling and leak checks
#   - python3:         test harness and benchmark tooling
#   - yyjson 0.12.0:   JSON library, built from source (no apt package)
#
# Idempotent and non-interactive: safe to re-run, installs nothing that is
# already present.
set -euo pipefail

# Only run in the remote (Claude Code on the web) environment; local
# machines are expected to provide their own toolchain.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

YYJSON_VERSION=0.12.0

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
# Best-effort refresh: the base image may carry unreachable third-party PPAs
# (e.g. deadsnakes, ondrej/php) that we don't need. Their failures must not
# abort the hook; the install step below is the real gate.
$SUDO apt-get update || true
$SUDO apt-get install -y --no-install-recommends \
    build-essential \
    flex \
    bison \
    cmake \
    valgrind \
    python3 \
    git

# Build and install yyjson from source if it is not already present. Pinned
# to the same release as the dev container for reproducibility.
if [ ! -f /usr/local/include/yyjson.h ]; then
    tmpdir="$(mktemp -d)"
    git clone --depth 1 --branch "${YYJSON_VERSION}" \
        https://github.com/ibireme/yyjson.git "${tmpdir}/yyjson"
    cmake -S "${tmpdir}/yyjson" -B "${tmpdir}/yyjson/build"
    $SUDO cmake --build "${tmpdir}/yyjson/build" --target install
    $SUDO ldconfig
    rm -rf "${tmpdir}"
fi
