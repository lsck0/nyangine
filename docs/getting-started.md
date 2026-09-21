# Getting started

## Clone and bootstrap

The engine vendors everything it needs, so a clone has to be recursive.

```bash
git clone --recursive https://github.com/lsck0/nyangine.git
cd nyangine
```

The build system is a C program that compiles itself. Bootstrap it once:

```bash
clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix \
    -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```

From then on `./build` recompiles itself whenever its own sources change. Run it bare to see
everything it can do:

```bash
./build
```

The first run builds every vendored dependency, which takes a while. After that it is incremental.

## Run something

```bash
./build run dev                       # the example game, optimized, hot reloading
./build run example hello_world       # the smallest thing that draws
./build run test                      # the test suite
```

## The modes

| Mode | For | Sanitizers | Assets | Hot reload |
| :--- | :--- | :--- | :--- | :--- |
| `debug` | Something is wrong in the code; find it | On | Filesystem | Yes |
| `dev` | Ordinary development | Off | Filesystem | Yes |
| `release` | What users get | Off | Bundled | No |

`debug` and `dev` both produce perf data. `release` has submodes: plain release is native, `steam` is
release through Steam, and flatpak, pacman, AUR, scoop and nix are the packager ones.

Assertions stay enabled in **every** mode, shipping included. A release build with its assertions
compiled out has stopped checking itself at the moment that matters, and `-DNYA_NO_ASSERT` is a hard
compile error rather than an option.

## Targets

Linux builds everything, including the Windows and Steam targets, by cross compiling. Windows builds
only the Windows targets.

```bash
./build build debug-linux
./build build release          # every release target except steam-linux
./build build steam-linux      # separate: downloads the sniper SDK sysroot once
./build dist                   # stage distributions under dist/
```

## The examples

Each is one self-contained `main.c` under `examples/`, run with `./build run example <name>`.

| Example | Shows |
| :--- | :--- |
| `hello_world` | The smallest program that opens a window and draws |
| `cli_tool` | No window at all: the engine as a command line program |
| `net_echo` | A server and a client over the UDP transport |
| `plugin_scripting` | Embedding Lua |
| `tui_dashboard` | Text output |

## Where to go next

- [Architecture](architecture.md) — how the pieces fit, and what a "system" is.
- [Cheatsheet](CHEATSHEET.md) — every public declaration, generated from the headers.
