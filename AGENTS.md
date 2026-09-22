# AGENTS.md

What to know before touching this repository. The API itself is in
[docs/CHEATSHEET.md](docs/CHEATSHEET.md), which is generated from the headers and is the only list
of names that cannot be out of date. When the cheatsheet and this file disagree, the header wins.

## What this is

nyangine is written in C2Y, built with clang, rendered through SDL3's GPU API. It ships as one
build of the engine plus `gnyame`, a small game that exists to exercise every engine feature.

It started as a game engine. It is becoming the framework for all of its author's software: one stack
for enterprise level desktop applications, web applications and games, plus TUI and CLI programs, all
composing in one program. Priorities, in order when they conflict: security, privacy, stability,
performance, then lines of code. `TODO.md` states that scope, and its "Roadmap" section is the plan and
the order of work. Read it before choosing what to build. What is not scope: Android, a second game, and scaling a
server past one machine.

Style: data oriented procedural C. Plain structs and functions that transform them, arenas rather
than `malloc`, assertions kept in release builds, fixed capacities with the bound written down. The
full guide is at `~/.claude/skills/l-style/SKILL.md`; read it before writing code.

## Layout

```
build.c                bootstrap entry point; recompiles itself from then on
src/build/             the build system: rules, CLI, hooks, preprocessor passes (pp/)
src/nyangine/          the engine
src/gnyame/            the game that exercises it
src/generated/         written by the preprocessor passes; never edit by hand
src/main.c             the executable's entry point and the hot reload host
tests/nyangine/        one file per unit under test, mirroring src/nyangine
bench/                 benchmarks
examples/<name>/main.c one self contained example each
assets/                shaders, fonts, sounds, locales, config
vendor/                third-party submodules, built by ./build, never edited in place
packaging/             AUR, Flatpak, winget, scoop and SteamPipe manifests
plugins/<name>/        Lua plugins: manifest.nya, main.lua, src/, assets/
docs/CHEATSHEET.md     generated API reference
docs/lua/nya.lua       generated Lua definitions for the `nya` table
docs/*.md              prose per subject, published through GitBook
```

Engine modules, each a directory under `src/nyangine/` with a `<module>.h` that includes the rest:

| Module     | What is in it                                                               |
| :--------- | :-------------------------------------------------------------------------- |
| `base`     | arenas, strings, arrays, dicts, logging, errors, assertions, files, hashing   |
| `platform` | clock, filesystem, process spawning, signals, raw memory, the terminal        |
| `math`     | scalars, vectors, matrices, quaternions, shapes, noise, random, springs       |
| `crypto`   | hashes, HMAC, XChaCha20-Poly1305, X25519, Ed25519, Argon2id, base32; monocypher |
| `core`     | the app loop, entities, systems, events, input, audio, assets, config, saves  |
| `renderer` | 2D and 3D drawing, cameras, text, particles, post processing, three backends  |
| `ui`       | immediate mode widgets                                                       |
| `physics`  | Box2D and Box3D behind one interface                                         |
| `net`      | encrypted UDP client and server, snapshots, prediction                        |
| `http`     | an HTTP/1.1 server, routing and layers, JWT, OpenAPI generated from both       |
| `serde`    | one dynamic value type, to and from json, jsonc and `.nya` (text and binary)  |
| `nn`       | tensors, layers, optimizers, DQN, NEAT                                       |
| `debug`    | the overlay, the trace, the crash window, and drawing physics and networks   |
| `plugins`  | optional dependencies behind a flag: curl, sqlite, lua, discord, steam        |

Not in the engine yet, and planned in `TODO.md`'s roadmap: the module layering and the component
system (every module above `base`, `platform` and `math` added or removed by one line in
`assets/config/plugins.nya`), a wasm target with a WebGPU renderer and a DOM UI presenter, TLS, the
accounts, roles and sessions stack, and SQLCipher. Do not describe any of it as if it exists; the
layout above and the flags below are how the tree works today.

### The three 2D backends

`nyangine.c` compiles exactly one of them, so nothing carries another's code:

| Flag             | File                                | What it does                                   |
| :--------------- | :---------------------------------- | :--------------------------------------------- |
| none             | `renderer/render2d.c`                | SDL's GPU API                                   |
| `-DNYA_HEADLESS` | `renderer/render2d_headless.c`       | nothing, exactly. What tests and benchmarks run |
| `-DNYA_TERMINAL` | `renderer/render2d_terminal.c`       | character cells, through `platform/terminal/`   |

`NYA_TERMINAL` implies `NYA_HEADLESS`: a terminal has no GPU device, no swapchain and no 3D. The
backends satisfy `render2d.h` and nothing above them changes, which is why `ui/ui_draw.c` is the
only file in the UI module allowed to name a drawing primitive — swapping backends is swapping one
file's call targets. A backend answers `render_features.h` for what it cannot do rather than
pretending; the terminal one asserts at open that it has left no switch unanswered.

## Building

**The build system is a C program in `src/build/`, driven by `./build`.** Not make, not cmake, not
a shell script. A new build rule, flag or command is a C edit in `src/build/`, and the tool
recompiles itself on the next run. `src/build/cli.c` holds the command tree; the per-host project
rules are under `on_linux/` and `on_windows/`; the preprocessor passes are in `pp/`.

Bootstrap it once from a fresh clone:

```bash
git submodule update --init --recursive
clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix \
    -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```

`./build` with no arguments lists every command. The first run builds every vendored dependency,
which takes tens of minutes; after that the artifacts are cached.

```bash
./build run debug              # sanitized
./build run dev                # optimized, no sanitizers
./build run release
./build run test [filter...]   # every test, or the ones whose path contains a filter
./build run bench [filter...]
./build run coverage
./build run example <name>     # builds examples/<name>/main.c and runs it
./build run simulation         # one deterministic simulation over the engine, from a seed
./build run agent --kind dqn   # a DQN or a NEAT population playing gnyame as a user, headless
./build build debug-linux      # what a change must still compile under
./build build release          # every release target this host can produce
./build check --strict         # clang-tidy over the translation units; what CI runs
```

### Modes

| Mode      | Meaning                                                  |
| :-------- | :-------------------------------------------------------- |
| `debug`   | Something is wrong in the code; find it. Sanitizers on.    |
| `dev`     | Ordinary development. Sanitizers off.                      |
| `release` | What users get. Optimized.                                 |

`debug` and `dev` both use filesystem assets with hot reload, and both produce perf data.
`release` has submodes: plain release is native, `steam` is release through Steam, and flatpak,
pacman, AUR, scoop and nix are the packager ones. Assertions stay on in every mode. In C the mode is
`NYA_EXECUTION_MODE`, readable as `NYA_DEBUG`, `NYA_DEVELOPER`, `NYA_RELEASE`, `NYA_STEAM` and
`NYA_TEST`.

### Platforms

**Linux compiles everything**: `./build build release` on a Linux host produces the Linux executable
and cross compiles the Windows and Steam Windows ones, and `build steam-linux` adds the sniper SDK
target. **Windows only builds the Windows ones** — `debug-linux`, `dev-linux`, `release-linux` and
`steam-linux` are not registered on a Windows host at all, and a Windows machine cannot compile the
shaders (DXC does not build under MinGW), so it needs `assets/shader/compiled/` from a Linux
machine. Verify on Linux.

### The editor's flags are hand maintained

`.clangd` carries its own copy of the compile flags, including every vendor include path and every
`-DNYA_PLUGIN_*`. **Add a vendor or a plugin flag to `src/build/flags.h` and you must add it to
`.clangd` too.** Nothing checks this: when the two disagree the editor silently analyses the tree
under the wrong flags, a whole module becomes an empty translation unit, and you edit it blind with
no diagnostics, no completion and no rename coverage. That has already happened three times. The
roadmap's component system generates `.clangd` from the build and ends this; until it lands, the rule
stands.

## Conventions

- Names are `subject_verb_object`, most significant part first, so everything about one type sorts
  together: `nya_arena_create`, `nya_entity_spawn`, `nya_render2d_rect`, `nya_string_push_back`.
- Engine symbols are `nya_` for functions and macros, `NYA_` for types, constants and enum members.
  The game's are `gny_` and `GNY_`. Anything prefixed `_nya_`/`_NYA_` is private.
- `NYA_API` marks a public declaration and is what the cheatsheet generator reads. `NYA_INTERNAL`
  is `static` plus hidden visibility and never appears in a public header.
- Every verb ships with its partner in the same header, adjacent: `create`/`destroy`,
  `init`/`shutdown`, `begin`/`end`, `push`/`pop`, `attach`/`detach`.
- Fallible calls return `NYA_Error` and are `__attr_no_discard`. `NYA_TRY(expr)` propagates,
  `NYA_EXPECT(expr, "context")` crashes through the crash sink with a backtrace.
- `defer` is C2Y's, from `<stddefer.h>` and `-fdefer-ts`, not a macro of ours. It fires at the end
  of the enclosing *block*, so a `defer` inside an `if` runs before the `if` does.
- Fixed capacity arrays register with `nya_ceiling_register` so the debug overlay can show how full
  they are.
- Files are grouped by domain and named thing-plus-kind, the prefix mirroring the directory:
  `core/core_entity.c`, `renderer/render2d_sprite.c`, `systems/system_camera.c`.
- Banner comments separate sections (`CONSTANTS`, `TYPES`, `FUNCTIONS`, `LIFETIME`, `INTERNAL`) and
  lower-level functions come before the higher-level ones that use them.
- Comments say why, never what. Inline comments are lowercase; doc comments (`/** */`) are prose.
- Conventional commits, imperative, lowercase, no trailing period: `type(scope): summary`. The body
  carries the reasoning, the rejected alternative and the measurement.

## Standing decisions

From `TODO.md`, which is the planning document and lives in the repository with the code:

| Area         | Decision                                                                                   |
| :----------- | :----------------------------------------------------------------------------------------- |
| Workflow     | Edit files directly. GitHub is a backup: push to preserve work, not as review or process.   |
| Comments     | Keep the why, cut the essay. One to three lines per prose block.                            |
| Gamepad      | `NYA_InputBinding` is a union of key, gamepad button, or axis past a threshold.             |
| Subsystems   | `core_system.h` registers engine subsystems and game systems alike.                         |
| Config       | `NYA_CONFIG` hot reloads from `assets/config/engine.nya`, backed by reflection.              |
| Ceilings     | Fixed capacity arrays register with `nya_ceiling_register`.                                 |
| Verification | Every feature gets a caller in `gnyame` or one of the examples, not only a test, and that caller runs in CI. |
| Data shapes  | Model (stored), optional SO (inside the program), DTO (on the wire). Only DTOs reach a client. |
| Servers      | One machine, one instance. TLS and simple rate limits in process; a proxy is optional.     |
| Programs     | Live in this tree beside gnyame for now.                                                   |
| Plugins      | `core_plugin.h` loads `plugins/<name>/`. Permissions are fixed at compile time by `-DNYA_PLUGIN_PERMISSION_PROFILE`. |
| Lua bindings | Generated from `@lua` annotations by `src/build/pp/luabind.c`. Never hand written unless they cannot be generated. |

The verification rule is load-bearing. **A feature with no caller in `gnyame` or an example is not
finished**, however green its tests are: this codebase is verified by running programs and looking at
them, and a test that passes against code nothing calls has proved very little. Add the caller in the
same change. gnyame is where the project lives and proves everything composes; each example in
`examples/` proves one kind of program alone.

`TODO.md` also carries what is unfinished per area and a findings section recording bugs and why
rejected approaches were rejected. Read the relevant entry before redesigning something.

## Generated files

`src/build/pp/` holds the preprocessor passes. Each is stale-checked, so it costs nothing when its
inputs have not moved, and each runs as part of an ordinary build:

- `reflection.c` → `src/generated/reflection.{h,c}`, from `@reflect` annotations in the tree.
- `i18n.c` → `src/generated/strings.h`, from `assets/i18n/*.json`.
- `asset.c` → `src/generated/assets.{h,c}`, the asset handles and the baked blob.
- `cheatsheet.c` → `docs/CHEATSHEET.md`, from the public headers.
- `luabind.c` → `src/generated/lua_bindings.c` and `docs/lua/nya.lua`, from `@lua` annotations.

Never hand-edit any of those outputs, `docs/CHEATSHEET.md` included: change the header and rebuild.

## Before you call it done

```bash
./build build debug-linux
./build run test
./build check --strict
```

All three must pass, and the feature must have a caller in `gnyame` or an example. Every commit compiles and
passes the tests on its own.
