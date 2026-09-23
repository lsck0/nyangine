# Architecture

## The shape of a nyangine program

Data oriented procedural C. Plain structs and functions that transform them; no hidden state, no
object pretending to be data. Arenas rather than `malloc`, fixed capacities with the bound written
down and asserted, assertions kept in release builds.

`main` is a composition root and nothing else: parse arguments, read config, probe the environment,
bring subsystems up in order, run, shut down.

## Modules

Each is a directory under `src/nyangine/` with a `<module>.h` that includes the rest of it.

| Module | What is in it |
| :--- | :--- |
| `base` | Arenas, strings, arrays, hashing, logging, reflection, errors, testing, build metadata |
| `platform` | The per-OS half: clock, filesystem, command, memory, signals, IPC, host probing |
| `core` | App loop, systems, entities, windows, input, assets, audio, config, save, scenes, social |
| `math` | Vectors, matrices, quaternions, noise, shapes, tweens, random |
| `renderer` | 2D and 3D drawing, post chain, shadows, particles, text, fluids, feature switches |
| `ui` | Immediate mode UI, split by domain across seven files |
| `physics` | Box2D and Box3D behind one interface, with collision layers |
| `net` | UDP transport, encryption, snapshots, prediction, chat |
| `http` | An HTTP/1.1 server, routing and layers, JWT, OpenAPI generated from both |
| `serde` | Text to and from a dynamic object tree, plus the reflection bridge |
| `testing` | Property tests, deterministic simulation, sessions, agents |
| `nn` | Tensors, DQN, NEAT |
| `debug` | Overlay, tracing, the crash reporter |
| `db` | One database file: bound statements, a reflected struct as a row, derived migrations |
| `plugins` | curl, Lua, Discord, Steam, each behind a compile flag |

`src/gnyame/` is a small game that exists to exercise every engine feature. The rule is that a
feature gets a caller there, not only a test — verification means running it.

## Systems

Everything that does per-frame work is a system in one registry: the engine's subsystems, the game's,
and a plugin's alike. A system is a name, an optional predecessor, an `init`/`deinit` pair, and a
callback per phase.

Three phases run each frame, in order:

| Phase | When | Delta |
| :--- | :--- | :--- |
| `FRAME` | Once, before events are handled | Variable |
| `TICK` | The fixed timestep, zero or more times per frame | Fixed |
| `RENDER` | Once, drawing | Variable |

```c
void gravity_tick(f32 delta_time_s) { ... }

nya_system_register((NYA_SystemEntry){
    .name  = "gravity",
    .after = "physics2d",
    .tick  = nya_callback(gravity_tick),
});
```

The registry owns the order and can be changed while the game runs. Disabling a system stops its
phase callbacks without touching its lifetime:

```c
nya_system_disable("gravity");   // for an effect; it stays registered and initialized
nya_system_enable("gravity");
```

That is the mechanism by which a game turns off an engine system, and by which a plugin replaces one.

**Callbacks, not raw pointers.** Entries hold `NYA_CallbackHandle` and resolve through the callback
registry on every use. A raw pointer into a hot-reloaded library keeps running the generation that
registered it; a handle re-resolves. The same reason is why the registry copies system names into its
own storage rather than holding a literal that lives in the image being replaced.

**Mutation during a run queues.** Registering or disabling from inside a phase callback applies when
that run ends, not in the middle of the iteration.

## Frames and ticks

Update runs at a fixed tick; frames draw whenever the display allows. A frame can land between ticks,
or see none at all. `nya_app_tick_alpha` says where between the last tick and the next it sits, and
entities capture their transform at the start of each tick so drawing can interpolate.

This is why particles draw from their previous tick's position and age toward the current one. Before
that, they froze on frames without a tick and jumped on the next, which reads as flicker.

## Configuration

`NYA_CONFIG` hot reloads from `assets/config/engine.nya`, backed by reflection — a field added to a
config struct appears in the file with no parsing code written. Defaults live in code and are
complete: the engine runs with no config file at all.

## Memory

Arenas are the default. Allocate at startup, then stop; steady state allocates nothing. Every arena is
introspectable — used, capacity, peak, resident — and the debug overlay shows all of it beside process
RSS.

Fixed capacities register with `nya_ceiling_register`, which is what makes the overlay able to show
every limit in the process sorted by how full it is.

## Reflection

One generated table per annotated type drives everything generic over a struct it was not written
for: config, scenes, saves, serialization, the property views. Generated from `// @reflect`
annotations in the source, never from a separate schema, and emitted as `const` data so there is no
startup cost.

```c
// @reflect
typedef struct { f32 gravity; u32 substeps; } MyOptions;
```

## Lambdas

A callback can be written where it is handed over. The preprocessor reads every
`nya_lambda(tag, ReturnType, (params), { body })` before anything compiles and writes the body out as
a real function, which the file that wrote it includes back in; the macro expands to that function's
name, so what reaches the call is a plain function pointer.

```c
nya_sim_defer(nya_lambda(reset_score, void, (void* data), { *(u32*)data = 0; }), &score, sizeof(score));
```

It cannot capture, and that is why it is safe: the function is at file scope, so naming a local of the
enclosing function is a compile error rather than a pointer into a frame that has already returned.
Callbacks here are stored, queued or called from another thread, and all three outlive the expression
that made one.

## Crashes

An assertion, a panic, a thrown error and a hardware fault all arrive at one sink, which composes one
report: what happened, the stack, the build, the machine, what the program's variables held, and the
last few hundred log lines. Everything but a fault opens a window with it; a fault, a headless run and
a test write it to a file beside the log and name it on stderr.

Two things put values in that report, and both print a value the same way.

A comparison assertion carries its operands:

```c
nya_assert_eq(written, expected);   // written == expected, where written is 6 and expected is 8
```

And a function can ask for its locals to be in the report, which costs a handful of stores per call
and nothing at all to a function that does not ask:

```c
// @watch
u32 build_row(u32 sides, u32 segments) {
    u32 at = 0;
    nya_watch(build_row);   // the parameters and everything declared above this
    ...
}
```

The preprocessor writes the registration into a companion header the file includes: each local's name,
its type as it was written and its address go into a fixed per thread ring, and a `defer` takes them
out again on every path out of the function, which is what keeps the report from reading a frame that
has already returned. The crash path walks that ring innermost frame first, without allocating and
without taking a lock, so a fault handler can walk it too.

```
Watched values
  child_stone_face
    u32 at = 8
    u32 wanted = 12
    u32 emitted = 11
  child_stone_row
    u32 sides = 4
    NYA_ConstCString shape = "rectangle"
```

Not DWARF: reading a variable out of the debug information needs a register context per frame and an
expression evaluator, and in a release build the locations are gone. See `base_watch.h`.

## What to read next

- [The HTTP server](http.md) for the one module with a page of its own so far.
- [Cheatsheet](CHEATSHEET.md) for the signatures.
- The headers themselves for the reasoning — each opens with what the module is for, every function
  in it, an example, and what was rejected.
