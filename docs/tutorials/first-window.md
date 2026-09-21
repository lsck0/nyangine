# Your first window

The smallest example, `examples/hello_world`, opens no window at all — it logs a line and exits,
which is the smallest thing that proves the engine links. This page goes from there to something on
screen.

Run the example first, to check the toolchain:

```bash
./build run example hello_world
```

## The entry point

```c
#include "nyangine/nyangine.h"
#include "nyangine/nyangine.c"

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);

    // First thing in the process: from here on every assertion, panic, thrown error and hardware
    // fault is captured with a stack trace and routed through the central crash sink.
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init());

    NYA_WindowHandle window = nya_window_create("first window", 1280, 720, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(window));

    nya_app_run();
    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
```

`nya_backtrace_init` comes first for a reason: until it runs, a crash is just a signal. After it,
every assertion and fault goes through one funnel, and in a build with the crash reporter that means
a window with the log leading up to the failure and a report the player can copy.

`nya_app_run` owns the loop. It pumps events, runs the fixed tick zero or more times, draws, and
presents. You do not write a loop.

## Drawing something

Drawing happens in a **layer**: a set of hooks pushed onto a window's stack. The stack is what makes
a pause menu a thing you push rather than a flag you check.

```c
void my_layer_on_create(NYA_Window* window)                   { nya_unused(window); }
void my_layer_on_destroy(NYA_Window* window)                  { nya_unused(window); }
void my_layer_on_event(NYA_Window* window, NYA_Event* event)  { nya_unused(window, event); }
void my_layer_on_update(NYA_Window* window, f32 delta_time_s) { nya_unused(window, delta_time_s); }

void my_layer_on_render(NYA_Window* window) {
    nya_render2d_rect(window, 100.0F, 100.0F, 200.0F, 120.0F, NYA_COLOR_WHITE);
}

nya_layer_push(window, nya_layer_of(my_layer, "my_layer"));
```

`nya_layer_of` derives all five hook names from the one prefix, so a layer cannot be wired to another
layer's `on_update` by a paste mistake — a missing hook fails to compile and names itself. A layer
that wants fewer hooks builds the `NYA_Layer` struct by hand instead.

Layers draw bottom to top and receive events top down, so the topmost layer sees an event first and
can consume it.

## Where the frame goes

`on_render` runs once per frame. Work that must happen at a fixed rate goes in `on_update`, which
runs zero or more times per frame depending on how much real time passed — see
[Architecture](../architecture.md) for why, and what `nya_app_tick_alpha` is for.

## Next

- [Drawing a UI](drawing-a-ui.md) — buttons, panels and layout instead of raw rectangles.
- [Adding a system](adding-a-system.md) — work that is not tied to one window.
