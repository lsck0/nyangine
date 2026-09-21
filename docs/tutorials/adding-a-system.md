# Adding a system

A layer belongs to a window and draws. A **system** is work that belongs to the program: physics
stepping, network ticking, an autosave timer, a plugin's behaviour. Engine, game and plugin systems
all live in one registry, which owns the order and runs the phases.

## Registering one

```c
NYA_INTERNAL_CALLBACK void gravity_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    // ... per fixed tick work ...
}

nya_system_register((NYA_SystemEntry){
    .name  = "gravity",
    .after = "physics2d",
    .tick  = nya_callback(gravity_tick),
});
```

`after` is an ordering edge, not a parent. `nya_system_registry_finalize` sorts by those edges into
run order and reports a bad graph rather than picking an order and hoping.

## The three phases

| Phase | When | Delta | Use for |
| :--- | :--- | :--- | :--- |
| `.frame` | Once, before events | Variable | Polling, clocks, anything per displayed frame |
| `.tick` | Fixed timestep, 0..n times per frame | Fixed | Simulation, physics, networking |
| `.render` | Once, drawing | Variable | Drawing that is not a window's layer |

A system supplies whichever it needs, and `init`/`deinit` for setup and teardown.

## `NYA_INTERNAL_CALLBACK` is not decoration

A function registered with the registry must be findable after a hot reload. `dlsym` finds neither a
`static` symbol nor a hidden one, so anything the registry resolves is marked
`NYA_INTERNAL_CALLBACK`: internal where nothing reloads, visible where something does.

This is the same reason the registry stores `NYA_CallbackHandle` rather than a function pointer, and
copies a system's name into its own storage rather than holding a literal. A raw pointer into a
library that has been replaced keeps running the old generation; a name literal points into the image
that was unmapped.

## Turning one off

```c
nya_system_disable("gravity");   // stays registered and initialized, stops ticking
nya_system_enable("gravity");
```

This is how a game disables an engine system to get a deliberate effect, and how a plugin replaces
one: disable the engine's, register its own.

Registering or disabling from **inside** a phase callback queues and applies when that run ends, so
the iteration in progress is never mutated under itself.

## Seeing what is running

`nya_system_registry_report()` logs the whole schedule, one line per phase, in run order. The debug
overlay's systems page shows the same live, with each system's owner, its phases, whether it is
enabled, and what it cost last frame — and lets you toggle one by hand. Turning `physics2d` off there
gives you a freeze frame with everything else still running.

Per-system timing is opt-in (`nya_system_accounting_enable`) because measuring costs something; the
overlay turns it on only while its page is open.

## Ownership

Every entry carries an owner — engine, game, or a named plugin. That is what makes per-plugin
accounting possible: `nya_system_owner_stats_at` gives one owner's system count, frame time and held
bytes, so "this plugin costs 0.4 ms a frame" is a question with an answer.

## Next

- [Architecture](../architecture.md) for how systems relate to frames, ticks and interpolation.
- `src/nyangine/core/core_system.h` for every function and its reasoning.
