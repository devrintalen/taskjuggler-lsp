FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Build/runtime dependencies for taskjuggler-lsp:
#   - build-essential: gcc + make
#   - flex / bison:    lexer and grammar generation
#   - cmake:           used to build yyjson from source
#   - valgrind:        callgrind profiling and leak checks
#   - python3:         test harness and benchmark tooling
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        build-essential \
        flex \
        bison \
        cmake \
        valgrind \
        python3 \
        git \
    && rm -rf /var/lib/apt/lists/*

# Build and install yyjson from source (matches CI; no apt package is used so
# the version is pinned to upstream main). Installs to /usr/local.
RUN git clone --depth 1 https://github.com/ibireme/yyjson.git /tmp/yyjson \
    && cmake -S /tmp/yyjson -B /tmp/yyjson/build \
    && cmake --build /tmp/yyjson/build --target install \
    && ldconfig \
    && rm -rf /tmp/yyjson
