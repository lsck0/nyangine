{ pkgs, lib, config, inputs, ... }:

# A reproducible toolchain, pinned by nix, for anyone who would rather not chase the exact
# clang the engine needs by hand. It is an OPTION and nothing more: the from-source bootstrap in
# README.md still works on a machine with a recent distro clang, mold and the -dev packages in
# .github/ci-packages.txt. What this file buys is that `direnv allow` (or `devenv shell`) drops
# you into a shell where `clang` is exactly the version CI builds with and every tool the build
# and its gates call is present, whatever the host distribution ships.
#
# There is no `languages.c` here on purpose. devenv's C module pulls an unpinned gcc/clang, and the
# whole reason this file exists is that the engine is fussy about the compiler: it needs a clang new
# enough for C2Y (`-std=c2y`), `-fdefer-ts`, `-fenable-matrix` and the `_Float16` half floats behind
# `-mf16c`. apt.llvm.org's clang-22 is what CI and the deploy path settled on, and mixing versions is
# exactly the fragility this replaces, so the toolchain is pinned by hand to LLVM 22 below.

{
  packages = [
    # ── The compiler, pinned. ──────────────────────────────────────────────────────────────────
    # LLVM 22, the version .github/ci-packages.txt installs from apt.llvm.org and .github/actions/
    # setup-linux asserts (`clang --version | grep "clang version 22."`). The nixpkgs input is the
    # devenv rolling channel (see devenv.yaml), which carries llvmPackages_22; the major is what the
    # engine's C2Y flags need, and holding it fixed is the point.
    #
    #   .clang        clang / clang++ / clang-cpp — the wrapped driver the build calls unversioned.
    #   .clang-tools  clang-format (asset.c) and clang-tidy (check.c --strict), both called by bare name.
    #   .compiler-rt  the sanitizer/profile runtime the coverage build links (apt: libclang-rt-22-dev).
    pkgs.llvmPackages_22.clang
    pkgs.llvmPackages_22.clang-tools
    pkgs.llvmPackages_22.compiler-rt

    # llvm-cov and llvm-profdata (test.c's coverage gate, `./build coverage`), plus llvm-ar/-ranlib/
    # -strip that the make-based vendors reach for. They ship with clang; here that means llvm_22.
    pkgs.llvm_22

    # ── Linkers. ───────────────────────────────────────────────────────────────────────────────
    # mold is the debug/dev linker on Linux (FLAGS_DEBUG_LINUX_X86_64: -fuse-ld=mold, flags.h). lld is
    # the release and host-native linker and the one the cmake vendors use (-fuse-ld=lld). clang finds
    # either by name on PATH once -fuse-ld= names it.
    pkgs.mold
    pkgs.lld_22

    # ── Vendor build tools. ──────────────────────────────────────────────────────────────────────
    # The vendored dependencies (src/nyangine-build/vendor/*) configure and build through these: cmake+ninja
    # for SDL and friends, make for lz4/sqlite/luajit/libbacktrace, nasm for the assembly in a couple
    # of them, pkg-config for the system libraries below. ccache is the compiler cache the split
    # rules launch through (COMPILER_CACHE_PROGRAM, flags.h); harmless when unused.
    pkgs.cmake
    pkgs.ninja
    pkgs.gnumake
    pkgs.nasm
    pkgs.pkg-config
    pkgs.ccache

    # ── Gates and packaging helpers. ─────────────────────────────────────────────────────────────
    pkgs.typos          # `./build typos`, the spell-check gate (github.com/crate-ci/typos, .typos.toml).
    pkgs.gettext        # envsubst, the templating a few packaging scripts use (apt: gettext-base).
    pkgs.git            # submodules and the version stamp the hooks read.
    pkgs.git-lfs        # the LFS-tracked assets a full checkout pulls.
    pkgs.zip            # `./build dist` zips the release archives (dist.c).
    pkgs.osslsigncode   # signs the Windows binary in the release path (hooks.c). Idle on a normal build.

    # ── System libraries the vendored SDL stack builds against. ──────────────────────────────────
    # nyangine vendors SDL3 and its companions from source, so their cmake builds still need the
    # host's windowing, GL, audio and text-shaping headers — the long -dev list in ci-packages.txt.
    # These are the nix equivalents; pkg-config (above) is what SDL's cmake finds them through. A
    # headless server checkout that never builds SDL does not need any of this, but carrying it keeps
    # `./build build vendor` working out of the box.
    pkgs.openssl.dev    # vendor_openssl.h links the *system* OpenSSL rather than vendoring one.

    # ── HTTP response compression, linked on every host build. ───────────────────────────────────
    # flags.h FLAGS_LINUX_X86_64 puts -lz -lbrotlienc -lbrotlicommon on every host link — the build
    # tool that rebuilds itself included, and gnyame at runtime. Those become DT_NEEDED entries the
    # loader resolves against the $ORIGIN-only rpath plus LD_LIBRARY_PATH and nothing else. Outside
    # this shell the host's /usr/lib carries them; inside, nix keeps them in the store, so without
    # these two packages (and the LD_LIBRARY_PATH below) a self-rebuilt `./build` dies with
    # `libbrotlienc.so.1: cannot open shared object file`. libz is here for the same reason.
    pkgs.brotli
    pkgs.zlib

    pkgs.alsa-lib
    pkgs.libpulseaudio
    pkgs.pipewire
    pkgs.jack2
    pkgs.sndio
    pkgs.nas
    pkgs.dbus
    pkgs.libdecor
    pkgs.libdrm
    pkgs.mesa
    pkgs.libGL
    pkgs.libglvnd
    pkgs.vulkan-loader  # libvulkan.so.1 — SDL_GPU's Vulkan backend loads it at runtime.
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.wayland-scanner
    pkgs.libxkbcommon
    pkgs.fribidi
    pkgs.libthai
    pkgs.ibus
    pkgs.liburing
    pkgs.udev
    pkgs.libx11
    pkgs.libxcursor
    pkgs.libxext
    pkgs.libxfixes
    pkgs.libxi
    pkgs.libxrandr
    pkgs.libxscrnsaver
    pkgs.libxtst

    # ── wasm (emscripten). ───────────────────────────────────────────────────────────────────────
    # emcc is off the engine's default toolchain — only `./build wasm` reaches for it, and flags.h says
    # as much (EMCC, FLAGS_WASM). It packages fine, with one wrinkle nix cannot hide: emscripten's port
    # cache lives inside the read-only store, so the first `emcc` has nowhere to build its sysroot. The
    # env block below points EM_CACHE at a writable per-project directory to fix that.
    pkgs.emscripten
  ];

  env = {
    # THE load-bearing setting. nixpkgs' cc-wrapper injects hardening flags (-D_FORTIFY_SOURCE=2,
    # -fstack-protector-strong, -Wformat-security, …) into every clang invocation. The engine's debug
    # and test builds are unoptimized (-ggdb, no -O), and _FORTIFY_SOURCE at -O0 makes glibc emit a
    # `#warning` that -Werror (WARNINGS in flags.h) promotes to a hard error — so a stock nix clang
    # would fail to compile the tree the moment you ran `./build`. Emptying the list makes the wrapped
    # clang inject nothing, so the only flags in play are the engine's own. This also matches the
    # plain distro clang the from-source bootstrap and CI use, which inject no hardening either.
    NIX_HARDENING_ENABLE = "";

    # A writable emscripten cache, out of the read-only store. It lives under .devenv/, which devenv
    # manages and which is never committed here (only the four config files are tracked). Only `./build
    # wasm` ever populates it; a first run builds the wasm sysroot here and later ones reuse it.
    EM_CACHE = "${config.devenv.root}/.devenv/emscripten-cache";

    # The engine builds every binary with a $ORIGIN-only rpath (flags.h), so the loader finds no system
    # .so on its own. Outside this shell the host loader's default path (/usr/lib …) resolves libz,
    # libbrotli* and libssl/libcrypto; inside it, nix keeps them in the store and off any default path,
    # so name their lib outputs here or a self-rebuilt `./build` — and a running gnyame — cannot load
    # them. openssl is already a package above (its .dev); this reaches its runtime lib output too.
    # stdenv.cc.cc.lib carries libstdc++.so.6, which the build tool needs through the SPIRV-Cross shared
    # library it links (`-lspirv-cross-c-shared`, a C++ library) for the shader-compile rules.
    #
    # The rest is what a windowed gnyame dlopens at runtime — SDL3 loads its video, input and audio
    # backends by soname (libwayland-client, libxkbcommon, libdecor, the Xlib set, libGL, and the ALSA/
    # PulseAudio/PipeWire trio), and SDL_GPU loads Vulkan (libvulkan). None are on the $ORIGIN rpath, so
    # without them here SDL_Init reports "wayland not available" and a GUI run dies at startup. On a
    # non-NixOS host the GPU *driver* (the Vulkan ICD, mesa's DRI) is still the host's, so a GUI/GPU run
    # may need to happen outside this shell (the nixGL problem) — the shell covers the build, the gates,
    # the tests and the headless server regardless.
    LD_LIBRARY_PATH = lib.makeLibraryPath [
      pkgs.brotli pkgs.zlib pkgs.openssl pkgs.stdenv.cc.cc.lib
      pkgs.wayland pkgs.libxkbcommon pkgs.libdecor pkgs.libdrm pkgs.mesa pkgs.libGL pkgs.libglvnd pkgs.vulkan-loader pkgs.dbus
      pkgs.libx11 pkgs.libxcursor pkgs.libxext pkgs.libxfixes pkgs.libxi pkgs.libxrandr pkgs.libxscrnsaver pkgs.libxtst
      pkgs.alsa-lib pkgs.libpulseaudio pkgs.pipewire
    ];
  };

  enterShell = ''
    echo "nyangine toolchain: $(clang --version | head -n1)"
    echo "linkers: mold $(mold --version | head -n1 | cut -d' ' -f2), $(ld.lld --version | head -n1)"
    echo "bootstrap with:  clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread"
    echo "then:            ./build"
  '';

  # ── What this shell deliberately does NOT provide ────────────────────────────────────────────────
  #
  # Windows cross-compilation (`./build build windows`, and the Steam/Windows release targets). It
  # needs mingw-w64, which nix has, but src/nyangine-build/on_linux/toolchain.h hardcodes the Debian sysroot
  # layout: CMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32 and the tools x86_64-w64-mingw32-windres /
  # -ar by their absolute distro names. nix stages mingw under a store path with no /usr sysroot and
  # no unversioned cross tools on PATH, so wiring it in cleanly would mean editing the toolchain header
  # — off-limits here (this change adds config only). Cross-build on a host with distro mingw-w64
  # installed the usual way, or add it in a devenv.local.nix once the header takes a sysroot override.
  # Native Linux — every gate, the tests, the game, the examples, coverage — is fully covered above.
  #
  # git-hooks are intentionally omitted. nyangine already installs its own commit hooks through the
  # build system (src/nyangine-build/hooks.c) and runs the gates in CI; layering devenv's pre-commit on top
  # would duplicate them and drop an untracked .pre-commit-config.yaml into the tree. Add one in a
  # devenv.local.nix if you want it locally.
}
