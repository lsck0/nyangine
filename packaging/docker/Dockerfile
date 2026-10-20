# syntax=docker/dockerfile:1
#
# The web_server example as a container image. Two stages, and only the second ships: the builder
# compiles the example with `./build run example web_server --server` (release, assets baked in, dead
# code collected, stripped; see src/nyangine-build/example.c), the runtime stage is FROM scratch with
# that binary, the shared objects `ldd` names, and a writable /data. See this directory's README.
#
# The build context is the repository root, with the submodules checked out.

ARG ARCH_IMAGE=docker.io/library/archlinux:base-devel@sha256:d6979123107e7a9f52c2de19104966b5f809e13a4593017780bd8c0b140f80ff
# A dated Arch Linux Archive snapshot, so the packages (clang 23 among them) are the same on every build.
ARG ARCH_SNAPSHOT=2026/10/19

# -----------------------------------------------------------------------------
# BUILDER
# -----------------------------------------------------------------------------

FROM ${ARCH_IMAGE} AS builder
ARG ARCH_SNAPSHOT

# The SDL headers are here because web_server still compiles the whole engine graph and links the SDL
# vendor archives; --gc-sections then drops all of it, so the runtime stage carries no SDL. git is for
# DXC's version script, which calls it and falls back to a placeholder outside a repository.
RUN echo "Server = https://archive.archlinux.org/repos/${ARCH_SNAPSHOT}/\$repo/os/\$arch" > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm --needed \
        clang lld llvm mold cmake make ninja nasm pkgconf python git \
        openssl zlib brotli zstd \
        libx11 libxext libxfixes libxi libxrandr libxcursor libxss libxtst libxkbcommon \
        wayland wayland-protocols libdecor mesa libdrm \
        alsa-lib libpulse pipewire dbus systemd-libs \
    && pacman -Scc --noconfirm

WORKDIR /src
COPY . .

# The bootstrap line is the README's.
RUN clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix \
        -Wno-initializer-overrides -Wno-gcc-compat -Wno-psabi -I./ -I./src -DNYA_NO_SDL -lm -pthread \
    && ./build run example web_server --server

# `cp --parents` keeps each library at the path the loader looks for it; the ldd line without `=>` is
# the loader itself. /tmp is where sqlite spills a temporary file once a sort outgrows memory.
RUN mkdir -p /rootfs/tmp /data \
    && chmod 1777 /rootfs/tmp \
    && cp web_server.example /rootfs/web_server \
    && ldd web_server.example | awk '/=>/ { print $3 } /ld-linux/ { print $1 }' | sort -u \
       | xargs -I{} cp --parents -L {} /rootfs/ \
    && chown 65532:65532 /data

# -----------------------------------------------------------------------------
# RUNTIME
# -----------------------------------------------------------------------------

FROM scratch

COPY --from=builder /rootfs/ /
COPY --from=builder --chown=65532:65532 /data /data

# No /etc/passwd in a scratch image, so the user is a number: the distroless "nonroot" uid.
USER 65532:65532

# The save root resolves under XDG_DATA_HOME, so notes.db lands in the volume.
ENV XDG_DATA_HOME=/data \
    WEB_SERVER_ADDRESS=0.0.0.0 \
    WEB_SERVER_PORT=8000

VOLUME ["/data"]
EXPOSE 8000

# The binary probes itself: no curl in a scratch image. It reads WEB_SERVER_PORT like the server does.
HEALTHCHECK --interval=10s --timeout=5s --retries=3 --start-period=5s CMD ["/web_server", "--healthcheck"]

ENTRYPOINT ["/web_server"]
