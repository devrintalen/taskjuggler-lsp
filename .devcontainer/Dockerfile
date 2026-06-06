FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Build/runtime dependencies for taskjuggler-lsp:
#   - build-essential: gcc + make
#   - flex / bison:    lexer and grammar generation
#   - cmake:           used to build yyjson from source
#   - valgrind:        callgrind profiling and leak checks
#   - python3:         test harness and benchmark tooling
#   - ruby-full:       provides gem, used to install TaskJuggler (tj3 / tj3man)
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        build-essential \
        flex \
        bison \
        cmake \
        valgrind \
        python3 \
        ruby-full \
        git \
    && rm -rf /var/lib/apt/lists/*

# Install TaskJuggler so tj3 / tj3man are on PATH. tj3man is the definitive
# reference for keyword syntax used when working on the language server, and
# tj3 can validate fixtures. Distributed only as a Ruby gem; on Debian/Ubuntu
# RubyGems installs executables into /usr/local/bin, which is already on PATH.
# Pinned for reproducibility.
ARG TASKJUGGLER_VERSION=3.8.4
RUN gem install taskjuggler --version "${TASKJUGGLER_VERSION}" --no-document

# Build and install yyjson from source (no apt package is used). Pinned to the
# latest stable release for reproducibility. Installs to /usr/local.
ARG YYJSON_VERSION=0.12.0
RUN git clone --depth 1 --branch "${YYJSON_VERSION}" https://github.com/ibireme/yyjson.git /tmp/yyjson \
    && cmake -S /tmp/yyjson -B /tmp/yyjson/build \
    && cmake --build /tmp/yyjson/build --target install \
    && ldconfig \
    && rm -rf /tmp/yyjson
