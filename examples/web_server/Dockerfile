# syntax=docker/dockerfile:1
#
# A container image for the web_server example: the smallest thing that answers HTTP.
#
# Two stages, and only the second one ships. The builder brings up the toolchain nyangine needs — a
# very recent clang, since the engine leans on the matrix_type extension, `-fdefer-ts` for its `defer`
# statement, `-mf16c` for _Float16, and lld — bootstraps ./build, and produces the example as a
# release artifact: optimized, its assets baked into the binary, dead code collected by the linker,
# and stripped (see `--server` in src/nyangine-build/example.c). The runtime stage is FROM scratch and holds
# exactly the stripped binary, the shared libraries `ldd` says it needs, and a writable directory for
# the notes database — nothing else, no shell, no package manager, no distro.
#
#   docker build -t registry.lsck0.dev/web_server:latest -f examples/web_server/Dockerfile .
#   docker run --rm -p 8000:8000 registry.lsck0.dev/web_server:latest
#   curl -f http://localhost:8000/
#
# The build context is the repository root, not this directory: the builder needs the whole tree.

# ─────────────────────────────────────────────────────────────────────────────────────────────────
# BUILDER
# ─────────────────────────────────────────────────────────────────────────────────────────────────

FROM debian:trixie AS builder

# clang 22 and lld from apt.llvm.org — the engine needs a clang this new, and Debian's default is
# older. cmake, make and git build the vendored dependencies; libssl-dev is the system OpenSSL the
# `tls` module and curl link against; pkg-config and ca-certificates round out the vendor configures.
#
# The SDL build dependencies (X11, Wayland, Mesa, audio) are here because the example still compiles
# the full engine graph and so links the SDL vendor archives — core, http and crypto sit behind
# NYA_NO_SDL in nyangine.h today, so there is no headless engine build to target yet, and building
# shadercross (which the shader step needs) pulls SDL in regardless. The linker's --gc-sections then
# drops all of it from the shipped binary, which is why the runtime image below carries no SDL at all.
# mingw-w64 is here because `./build` brings up every vendor for both its targets before any command
# runs; the Windows cross-build is discarded with this stage.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release software-properties-common \
        cmake make git pkg-config libssl-dev \
        mingw-w64 \
        libx11-dev libxext-dev libxfixes-dev libxi-dev libxrandr-dev libxcursor-dev \
        libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libgl1-mesa-dev \
        libgles2-mesa-dev libegl1-mesa-dev libgbm-dev libdrm-dev libasound2-dev \
        libpulse-dev libudev-dev libdbus-1-dev \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 22 \
    && ln -sf /usr/bin/clang-22 /usr/bin/clang \
    && ln -sf /usr/bin/ld.lld-22 /usr/bin/ld.lld \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

WORKDIR /src
COPY . .

# Bootstrap the build tool exactly as the README does, then build the shaders and the asset blob (a
# release reads its web bundle out of that blob, so it has to exist), then the server example as a
# shipping artifact. `--server` builds it release + stripped and does not run it; see example.c.
RUN clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix \
        -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread \
    && ./build build assets \
    && ./build run example web_server --server

# Stage the binary and only the shared objects it actually resolves into a root that the scratch image
# adopts wholesale. `cp --parents` keeps each library at the path the loader looks for it, and the
# ldd line without `=>` is the dynamic loader itself. The data directory is where the notes database
# is created at runtime; XDG_DATA_HOME below points the save root at it.
RUN mkdir -p /rootfs/data \
    && cp web_server.example /rootfs/web_server.example \
    && ldd web_server.example | awk '/=>/ { print $3 } /ld-linux/ { print $1 }' | sort -u \
       | xargs -I{} cp --parents {} /rootfs/

# ─────────────────────────────────────────────────────────────────────────────────────────────────
# RUNTIME
# ─────────────────────────────────────────────────────────────────────────────────────────────────

FROM scratch

COPY --from=builder /rootfs/ /

# Where the notes database lands. The save root resolves under XDG_DATA_HOME, and /data is the one
# writable place a scratch image has; mount a volume here to keep the notes across a redeploy.
ENV XDG_DATA_HOME=/data

EXPOSE 8000

# The container's own liveness probe, and the reason the image needs no curl: the binary probes
# itself — a GET / on the loopback port through the client it already links — and exits zero only on a
# 200. Docker runs this inside the container, so on a FROM scratch image it is the one thing that can.
# A swarm stack that sets its own healthcheck (see deploy/stack.yaml) overrides this.
HEALTHCHECK --interval=10s --timeout=5s --retries=3 --start-period=5s \
    CMD ["/web_server.example", "--healthcheck", "--port", "8000"]

# 0.0.0.0, not the loopback default: nothing else is in the container to proxy for it, so the server
# is what faces the published port. See NYA_HttpConfig.address and this example's main().
ENTRYPOINT ["/web_server.example", "--address", "0.0.0.0", "--port", "8000"]
