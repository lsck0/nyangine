<div>
    <h1 align="center">
        nyangine
    </h1>
    <h3 align="center">
        Game Engine
    </h3>
</div>

Clone the repository recursively with

```bash
git clone --recursive http://github.com/lsck0/nyangine.git
```

or

```bash
git clone http://github.com/lsck0/nyangine.git
cd nyangine
git submodule update --init --recursive
```

and bootstrap the build system with

```bash
clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```

then run

```bash
./build
```

to get the available commands. `./build run debug` starts the game, `./build run test` runs the
tests, and `./build run example hello_world` builds and runs one of the examples in `examples/`.
The first build brings up every vendored dependency, which takes a while; after that they are
cached.

`./build coverage` builds the tests under clang's source-based coverage, runs them, and reports
per-file and total line coverage of `src/nyangine`. It exits non-zero when the total is below
`--fail-under` (default 45, a floor meant to be ratcheted up), so CI can call it as a gate;
`--html` also writes an annotated listing under `.coverage/html`. It needs `llvm-profdata` and
`llvm-cov`, which ship with clang, and skips with a notice rather than failing when they are absent.

## Reproducible toolchain (nix)

Getting the compiler right is the fiddly part of the bootstrap above: the engine needs a clang new
enough for C2Y, `-fdefer-ts`, `-fenable-matrix` and `_Float16`, and a distribution's default clang
is rarely it. If you have [nix](https://nixos.org) with [devenv](https://devenv.sh) and
[direnv](https://direnv.net), an optional pinned toolchain is one command away:

```bash
direnv allow    # or, without direnv: devenv shell
./build
```

That drops you into a shell with exactly the LLVM 22 clang CI builds with, plus mold, cmake, ninja,
make, the coverage and spell-check tools the gates need, and the system libraries the vendored SDL
stack compiles against — all pinned. `devenv.nix` documents the two things it does not cover
(Windows cross-compilation, which the toolchain header hardwires to a distro sysroot, and
emscripten's writable cache). This is purely an alternative to installing the packages in
[.github/ci-packages.txt](.github/ci-packages.txt) by hand; the from-source bootstrap above still
works without nix, and nothing in the build depends on it.

## Where things are

- [AGENTS.md](AGENTS.md) — the layout, the build modes, the conventions. Start here.
- [docs/CHEATSHEET.md](docs/CHEATSHEET.md) — every public declaration, generated from the headers.
- [TODO.md](TODO.md) — what is left, and why things are the way they are.
- [packaging/README.md](packaging/README.md) — cutting a release per channel.
- [secrets/README.md](secrets/README.md) — the signing key: how to make it, add it, and let CI decrypt it.

Real documentation lives above the declaration it describes; the headers under `src/nyangine` are
the manual.

## Dependencies

Dependencies are listed in ./.github/ci-packages.txt. Not all of those are mandatory for building.

## Windows

In theory, building works on Windows. In practice, I don't use Windows, so who knows. A Windows host
builds the Windows targets only, and cannot compile the shaders; a Linux host builds everything.

```bash
clang build.c -o build.exe -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread -lbcrypt
```
