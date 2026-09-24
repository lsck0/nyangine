# Splitting core out of the SDL wall

This is the plan for the biggest open layering item in TODO.md: letting `http`, `net` and a headless
server build without SDL or the renderer. Today the whole of `crypto`, `tls`, `physics`, `net`, `http`,
`core` and `reflection_engine` sit inside one `#ifndef NYA_NO_SDL` block in `nyangine.h`/`nyangine.c`, so
`-DNYA_NO_SDL` gives you `base`, `os`, `math`, `serde`, `crypto`'s headers, `tls`, `acme`, `permission`
and the plugins — and nothing above them. A server links the whole engine because it has no other choice.

The refactor is large and this document does not pretend one commit does it. It records what was
measured, names the seams, and lays out an order in which the wall can come down one non-breaking step at
a time. The first step — the `core_runtime.h` header this plan introduces — is done; the rest are marked.

## What was measured

Two things were checked directly rather than reasoned about, because the include graph is the whole
question here.

**Which core headers pull SDL.** Each `core_*.h` was preprocessed under `-DNYA_NO_SDL` with no SDL
include path, and the include tree `clang -H` prints was searched for an `SDL3/` line. A header that
pulls no `SDL3/` header is on the free side; one that pulls any is on the bound side. This is exact and
does not depend on whether the host happens to have SDL installed (Arch does, at `/usr/include/SDL3`,
which is why a bare compile is not the test — the tree is).

**What http and net depend on.** `http` includes no `nyangine/core/*` header at all, and its only
non-`base`/`os`/`net` includes are `crypto`, `serde` and one `permission`. Its `.c` files call no core
function — `nya_asset_read` and `nya_callback_get` appear only in doc comments. The coupling runs the
*other* way: `core_app.c` calls `nya_system_http_tick()` to drive http's drain from the frame loop, under
its own `#ifndef NYA_NO_SDL`. So http and net are inside the wall by *placement in the include list*, not
by dependency. `crypto`, `tls`, `acme` and `smtp` contain zero SDL references; `net` contains zero; `http`
contains exactly one, `SDL_Delay(1)` in `http_server.c` (a thread-join spin, left over from when threads
were SDL's), plus three now-unused `SDL3/` includes.

That second finding is the important one: the http/net half of "the server links the whole engine" is not
blocked by a real dependency on core. It is blocked by the wall being drawn around the whole list and by
the two facts below.

## The core inventory

### SDL-free today (the runtime floor — now `core_runtime.h`)

These pull no `SDL3/` header and name no renderer, window, asset, physics, entity, http, net or ui type.
They reach for `base`, `os`, `math`, `serde` and `crypto` only.

| Unit                         | What it is                                              |
| ---------------------------- | ------------------------------------------------------- |
| `core_types`                 | handles and the small shared enums                      |
| `core_callback`              | the callback token table                                |
| `core_system`                | the system registry (init/tick/deinit by owner)         |
| `core_sim`                   | the fixed-step simulation clock                          |
| `core_job`                   | the job pool (on `base_thread`)                          |
| `core_taskgroup`             | fan-out over the job pool                                |
| `core_app_entry`             | the `nya_app_entry_*` binary-selection contract         |
| `core_http_reload`           | the callback-token resolver the http router reloads through |
| `core_save`                  | the save root (an arena written through serde)          |
| `core_undo`                  | a reflected diff stack over a save                      |
| `core_plugin`                | the plugin registry (one entry per system)              |
| `core_plugin_signature`      | crypto over a plugin's bytes                            |
| `core_tween`                 | scalar/vector interpolation                             |
| `core_skeleton`(+`_inertial`,`_blend`,`_layer`) | pose hierarchy math                  |
| `core_audio`(+`_panner`,`_effects`,`_propagation`) | the audio model as declarations   |
| `core_keys`, `core_gamepad`  | scancode and button/axis name tables                    |

Note that `core_audio.c` and `core_gamepad.c` *do* call SDL — but their headers name none of it, so the
declarations belong on the free side and only the translation units stay bound. That split (header free,
`.c` bound) is exactly what a component facade will formalise later.

### SDL-bound today, and why

| Unit                | Bound by                                                                 |
| ------------------- | ------------------------------------------------------------------------ |
| `core_event`        | **the linchpin.** `SDL3/SDL_events.h`, `SDL_init.h`, `SDL_mutex.h`: an `SDL_Mutex*` field and the mouse-wheel enum values. |
| `core_mouse`        | `SDL3/SDL_mouse.h` for the wheel-direction enum                          |
| `core_input`        | includes `core_event`                                                     |
| `core_settings`     | includes `core_input` → `core_event`                                      |
| `core_i18n`         | includes `core_asset` (renderer) and `core_event`                        |
| `core_control`      | includes `core_event`                                                     |
| `core_config`       | includes `core_asset`, `core_event`, `http/http_log.h`, `ui/ui.h`        |
| `core_asset`        | `SDL3/SDL_gpu.h`, `SDL_image`, `SDL_mixer`, `SDL_ttf`, `renderer.h`      |
| `core_window`       | `SDL3/SDL_gpu.h`, `renderer.h`                                            |
| `core_entity`       | `render2d_sprite.h`, `physics2d/3d.h`                                     |
| `core_world`        | includes `core_entity`, `physics2d/3d.h`                                  |
| `core_scene`        | includes `core_entity`, `core_world`                                      |
| `core_terrain2d/3d`, `core_tilemap` | `renderer/render_color.h`                                 |
| `core_nav`          | includes `core_tilemap`                                                   |
| `core_social`       | `net/net_config.h` (not SDL, but above the free floor)                   |
| `core_app`          | `renderer.h`, `physics2d.h`, `core_window`, `core_entity`, `core_asset`, `core_config` |

### The two hard blockers

1. **`core_event` is a hub.** Events, input, settings, i18n and config all reach SDL only *through*
   `core_event.h`, and `core_event.h` needs SDL for two small things: one `SDL_Mutex*` field on the event
   queue and the `NYA_MOUSE_WHEEL_DIRECTION_*` enum borrowing `SDL_MOUSEWHEEL_NORMAL`/`_FLIPPED`. Neither
   is a real SDL dependency — `os/os_thread.h` already has a mutex, and the enum can name its own values.
   The whole events/input/settings/i18n/config cluster moves to the free side the day those two are cut.

2. **`NYA_App` embeds the renderer by value.** `NYA_App` (core_app.h) has `NYA_RenderSystem
   render_system;`, `NYA_AssetSystem asset_system;` and `NYA_WindowSystem window_system;` as *value*
   members, so the struct cannot be defined without the SDL/box2d headers — even though its
   `callback_system`, `job_system` and `save_system` members are SDL-free on their own. Every accessor
   that hands back `NYA_App*` therefore drags the renderer's headers in. This is why `wasm_ui.c` has to
   include `nyangine.h` whole even though it compiles almost none of it. Cutting this means either the
   heavy systems become pointers to storage the program places, or the runtime-core systems move off
   `NYA_App` into a smaller aggregate that a headless program owns without the renderer ones.

## The ordered plan

Each step is behaviour-preserving and leaves the tree green on its own. The include graph and the link
lines change; nothing a program does changes.

1. **[done] Name the free floor.** `core_runtime.h` includes exactly the SDL-free core headers above,
   and `core.h` includes it first and then everything else it always did. `#pragma once` makes the
   overlap a no-op, so every build sees the identical declaration set. This is the seam later steps aim
   at; it compiles under `-DNYA_NO_SDL` with no `SDL3/` header in its tree, proven by `clang -H`. It is
   not yet depended on by anything — that is steps 5 and 7.

2. **Take the last SDL call out of http.** Replace `SDL_Delay(1)` in `http_server.c` with
   `nya_os_time_sleep_ms(1)` (already the spin used in `base_rate.c`) and drop the three dead `SDL3/`
   includes. After this, `http`, `net`, `crypto`, `tls`, `acme` and `smtp` are SDL-free at the source
   level — nothing in them names SDL. (This step ships alongside step 1.)

3. **Cut `core_event` off SDL.** Replace the `SDL_Mutex*` field with the engine's own `NYA_Mutex`
   (`os/os_thread.h` via `base_thread.h`), and give `NYA_MouseWheelDirection` its own enum values with a
   translation at the SDL event pump in `core_event.c` (the pump stays bound; the header stops being).
   Do `core_mouse.h` the same way. Then `core_event`, `core_input`, `core_settings`, `core_control` and —
   once `core_i18n` and `core_config` shed their `core_asset`/http/ui includes — the whole events/input
   cluster joins the free floor. This is the step that makes the runtime core actually useful to a
   server: a system registry with events and config in it.

4. **[done] Lift http/net out of the wall, behind `NYA_SERVER`.** crypto, tls and acme were already
   above the block. With steps 2–3 done, net and http followed: `nyangine.h` now pulls them under
   `#if !defined(NYA_NO_SDL) || defined(NYA_SERVER)` rather than the SDL block. The guard is additive on
   purpose — `NYA_NO_SDL` still means "no networking" for the host build tool and the wasm demos (they
   set it and not `NYA_SERVER`, so they are byte-for-byte unchanged), and a full build gets net/http as
   before. What is new is `-DNYA_NO_SDL -DNYA_SERVER`: it compiles `base`/`os`/`math`/`crypto`/`tls`/
   `acme`/`net`/`http`/`db`/`accounts`/`serde`/`template` with **no** `core` and **no** renderer, and it
   compiles clean — the very thing the ground-truth note called impossible while http sat inside the SDL
   block. This retires "a CLI/server links the whole engine" for the http/net half at the compile seam.
   Still open on this axis: a headless *link* (step 6–7) — a server binary needs an app loop that ticks
   `nya_http_server_tick` itself rather than `core_app.c` doing it, and the `NYA_SERVER_VENDORS` link set.

5. **Give the runtime core a home off `NYA_App`.** Introduce the smaller aggregate the free systems live
   on (registry, callbacks, jobs, save, config, events), owned by a headless program directly, with
   `NYA_App` holding it plus the renderer/asset/window systems for a windowed program. Accessors for the
   free systems stop returning `NYA_App*`. This is the `NYA_App`-by-value blocker; it is the largest
   step and wants its own design pass (see the components section of TODO.md — this is where "everything
   is a plugin" and this split meet).

6. **Split the SDL-bound core into its own umbrella.** With the floor named and populated, the rest of
   `core.h` becomes `core_engine.h` (app loop, window, renderer glue, entity, world, scene, terrain,
   tilemap, nav) and `core.h` becomes `core_runtime.h` + `core_engine.h`. `NYA_NO_SDL` guards
   `core_engine.h` only.

7. **A headless server example builds under `-DNYA_NO_SDL`.** The proof the whole thing was for:
   `examples/web_server` (or a new one) compiles and links with no SDL vendor on the line, and the
   `NYA_SERVER_VENDORS_LINUX_X86_64` subset in `src/build/vendor/vendor.h` becomes the real link set
   rather than a link-time `--gc-sections` trick. This retires `NYA_NO_SDL` as "no core" and turns it
   into "these components are absent", which is the components refactor.

## Honest blockers and scope

- Steps 1 and 2 are pure wins and carry no risk; they are what this change ships.
- Step 3 is mechanical but touches a hot file (`core_event`) and the wasm build, which already gates that
  file's SDL under `OS_WASM`; it needs care that the native event pump stays byte-identical.
- Step 5 is the real architecture and cannot be done mechanically. `NYA_App` embedding the renderer by
  value is the wall's keystone: until the free systems live somewhere a program can own without the
  renderer's type being complete, any header that reaches `nya_app_get()` drags SDL in, and that is most
  of the engine's surface. This is deliberately left to the components design rather than forced here.
- `reflection_engine` stays inside the wall throughout: it describes types that only exist in an SDL
  build, so it moves only when the components refactor gives each component its own reflection table.
