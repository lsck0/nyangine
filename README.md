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
clang build.c -o build.exe -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```
