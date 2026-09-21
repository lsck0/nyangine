# nyangine: what is left

`[ ]` todo · `[~]` in progress · `[⏭]` deferred

---

## Where it stands

212 tests pass, `check --strict` reports nothing, and debug, release and steam-windows build. The title
screen logs one line in twenty seconds, where it logged 6813.

Landed since the scope widened: the build system reorganised with `./build dist`, a `secrets/` tree encrypted
with sops, and a generated changelog; an IPC control socket and a WebSocket client, both fuzzed; a crash
reporter with its own window; a cheatsheet generated from the headers and an `AGENTS.md`; collision layers,
AFL++ fuzzing, property tests and a deterministic simulation harness; Steam lobbies, peer to peer and
achievements, and Discord presence and invites behind one facade; scene persistence through reflection; the
shadow lag and the fire flicker fixed with measurements; the UI split into seven files with fixed scale and
eleven new widgets; and one system registry driving the frame for engine and game alike.

In progress now: fluids, the plugin system, the TUI backend, the HTTP server, and fast-forward with DQN
driving the game. The web client is the one large thing not started. See "The stack" and "Requested".

---

## Standing decisions

| Area         | Decision                                                                                     |
| :----------- | :------------------------------------------------------------------------------------------- |
| Workflow     | Edit files directly. GitHub is a backup: push to preserve work, not as review or process.     |
| Comments     | Keep the why, cut the essay. One to three lines per prose block.                             |
| Gamepad      | `NYA_InputBinding` is a union of key, gamepad button, or axis past a threshold.              |
| Subsystems   | `core_system.h` registers engine subsystems and game systems alike.                          |
| Config       | `NYA_CONFIG` hot reloads from `assets/config/engine.nya`, backed by reflection.               |
| Ceilings     | Fixed capacity arrays register with `nya_ceiling_register`.                                  |
| Verification | Every engine feature gets a caller in `gnyame`, not only a test. Verify by running the game. |

---

# Definition of done

The engine is finished when all of this holds, in the existing style (see the style guide), with lines of code,
file size, RAM, VRAM, CPU, GPU and startup time kept to a minimum, and nothing a player or a peer does can crash
it. gnyame stays a minimal example exercising every feature.

Scope was "a game engine". It is now **one stack for everything I write**: games, desktop UI, TUI, CLI,
web servers and web clients, all in the same program and all composing. One nyangine program should be able to
mix 2D and 3D rendering, put a UI over it, serve a web interface for its own metrics, accept messages from other
programs, talk to OBS over WebSocket, and be driven from a CLI or a TUI, with plugins, optional end to end
encryption and PGP-backed second factors. See "The stack" below for what that adds.

| Area | Wanted | State |
| :--- | :--- | :--- |
| 2D/3D renderer | animation, particles, atmosphere, liquids, opacity, reflections, dynamic LOD, eye adaptation | `[~]` animation, particles, fog, glass, terrain and mesh LOD, eye adaptation, light shafts, aerial perspective and motion blur exist; volumetrics, liquids and reflections missing |
| Post processing | a composable chain | `[x]` occlusion, ink, depth of field, FXAA, grade, bloom, speed lines, HDR output |
| Graphics options | antialiasing, motion blur, fov, ... toggleable | `[x]` MSAA, FXAA, shadows, post passes, fov and render scale are player settings, and 37 feature switches cover everything else including culling, sorting and the depth test |
| Renderer debug | physics hitboxes and other debug views | `[~]` buffer views exist; physics shapes missing |
| Audio | raytraced: occlusion, diffraction, echoes, room estimation; sound post processing | `[x]` partial occlusion, transmission, diffraction, room driven reverb, echo taps; per bus chain (filters, EQ, compressor, echo, reverb, limiter). Open: interaural delay and head shadow (needs our own panner instead of SDL_mixer's) |
| UI | immediate layout, styling, animation; widgets incl. colour picker, sliders, buttons, inputs; debug look by default, texture skins for game UI | `[x]` seven files by domain, fixed scale, nine-slice skins, full text editing with selection and clipboard, dropdowns, radio, tabs, draggable panels, tables, charts, icons, opacity groups, scrolling. Open: a floating dropdown, a node editor, SVG, the code editor widget |
| Core | events, entities, input, settings, cache, ... solid | `[x]` one system registry drives frame, tick and render for engine and game, with runtime enable/disable and per-owner accounting, and its entries are callback handles with copied names, so a system registered from a reloaded image survives the reload. Scenes and settings persist through reflection |
| Pipelines | build, assets, reflection | `[x]` |
| Hot reload | assets, code, configuration | `[x]` |
| Tracing | time and memory per feature (shadows, antialiasing, particles, ...) | `[~]` CPU spans, GPU allocation counters, and per-system and per-owner time and memory from the registry; per renderer feature attribution missing |
| CI/CD | tests and builds with caching | `[x]` green on Linux and Windows. `./build dist` stages every target, the changelog is generated, secrets are sops encrypted |
| Crash reporting | one funnel, a window a player can act on, everything a triage needs in it | `[~]` log ring, composed report (crash, build, machine, stack, log), its own SDL window with close, copy and send, and a file under the log directory. Open: a transport behind `nya_crash_report_submit`, and a window on the fault path (SDL from a signal handler can deadlock) |
| Anti-tamper | integrity checks like the CRC | `[x]` executable stamp, chunked code baseline and a sweep every 250 ms, per blob entry hashes, a watchdog at two inlined sites; failure logs and exits 86 |
| Networking | attack and cheat resistant, optional end to end public key encryption | `[x]` X25519 stateless handshake, XChaCha20-Poly1305 per packet, pinned server keys, rate limits, server authority with a violation score, delta snapshots, fuzzed decoders |
| Targets | Linux, Windows, Steam Linux, Steam Windows | `[x]` all four build; Steam Linux against the sniper SDK (glibc 2.31, GnuTLS). Web and TUI are wanted and not started; Android is out |

# Unmerged work

Everything from the parallel session is merged: the build reorganisation and `dist`, IPC and WebSocket, the
crash reporter, the generated cheatsheet and four examples, collision layers and the simulation harness,
Steam and Discord, scene persistence, the renderer bug fixes, the UI split and its widgets, and the system
registry, and the renderer feature switches. 212 tests pass, `check --strict` is clean, and debug, release
and steam-windows all build.

What is written and NOT merged, sitting as stashes in `.claude/worktrees/`:

- `[x]` `renderflags-wip-lc` landed: 37 feature switches including frustum culling, backface culling, draw
  sorting and the depth test, which had none. It carried no asset placeholders after all, so that item is
  still open under "Renderer".
- `[ ]` `serde-wip-lc` — hashing the reflection names in release builds, so `strings` on the binary stops
  handing over the whole type layout.
- `[ ]` `social-wip-lc` — the gnyame side of the join-request prompt (`layer_social.c`, `social.c`).
- `[ ]` `crashtest-wip-lc` — a test that opens the crash window and dismisses it from another thread.
- `[ ]` Two untracked example directories in the docs worktree: `pong_multiplayer` and `pinball3d`.

Each needs its branch rebased onto master, built, and `check --strict` run before it lands. Do not merge any
of them on the strength of having been written.

- `[ ]` Not started: debug draw and physics hitboxes, core systems audit.

---

# The stack

What "one stack for everything" adds on top of the engine. Nothing here exists yet unless it says so.

## `[ ]` Web

In scope, deliberately: not only a server, but the client too.

- `[ ]` An HTTP server in the engine: routing, middleware, typed request and response structs, JSON through
  `serde`. One GET or POST per path, accepting an enum. Middleware for auth and logging. JWT, plus the PGP
  pieces. Rate limits and the rest of the perimeter belong to a proxy in front, not to us.
- `[ ]` OpenAPI generated from the handler definitions and the DTO types, served by the app. Never hand written.
  See `~/projects/webapp-template` for the patterns to follow.
- `[ ]` Compile to web: a bundle of HTML, CSS, JS and wasm. WebGPU where it exists, a canvas backend otherwise.
- `[ ]` A UI backend that emits HTML, CSS and JS from the same `nya_ui_*` calls the native backend draws, ahead
  of time or on the fly. **I never write HTML, CSS or JS by hand.** That is the whole point of the exercise.
- `[ ]` Fully client side apps that talk to a nyangine server, with the types shared between the two rather than
  restated.
- `[ ]` Hot reloading on the web, matching what the native builds already do.

## `[ ]` TUI

- `[ ]` A terminal backend beside the GPU one: ncurses or equivalent, so a nyangine program can be a TUI that
  wraps something like `gh`. The headless renderer pair is the existing precedent for a second backend.
- `[ ]` Kitty image protocol, so a TUI can still show pictures.

## `[~]` IPC and talking to other programs

- `[x]` A local control socket, opt-in and off by default: `platform/ipc/` is a unix socket on Linux and a
  named pipe on Windows, and `core_control.c` is the surface an outside program drives the engine through.
  Every inbound byte is parsed at the boundary and the decoders are fuzzed (`test_fuzz_control.c`).
- `[x]` An outgoing WebSocket client over ws and wss (`plugins/curl/websocket.h`), framing, ping/pong and
  close, fuzzed. `[ ]` Nothing drives OBS with it yet, which is the point of having it.
- `[x]` An outgoing REST client exists (`plugins/curl/request.h`) but nothing calls it.
- `[ ]` An HTTP server, and OpenAPI generated from the handlers. Not started.

## `[ ]` ruey

`~/projects/ruey`, a Twitch client with integrations, gets rewritten into nyangine later. The WebSocket
client now exists; it still needs the HTTP server and the TUI backend. Not startable until those land.

---

# Requested

Everything asked for that is not already covered by a section below. Android is explicitly out of scope and is
not listed.

## `[ ]` Plugins

The model to copy is Dalamud's: a list of plugin repositories the user can add by URL, each serving a JSON
index, with per-plugin enable and disable, download counts, and a blunt warning that a plugin is arbitrary code
and can do anything the program can.

- `[ ]` Lua plugins loaded from `plugins/<name>/` with `manifest.nya`, `main.lua`, `src/*.lua` and `assets/`.
  The manifest carries author, license, plugin version, engine version, dependencies, conflicts, and a git repo
  URL so it can update itself.
- `[ ]` Custom plugin repositories: add a URL, it serves an index, plugins show up as installable.
- `[ ]` A plugin repo of our own, holding plugins as submodules, listed in-game as downloadable.
- `[ ]` Steam Workshop plugins. A plugin may be nothing more than a map or a character added to the selection.
- `[ ]` Parity between the Lua plugin API and what can be written in C. All engine code stays C; Lua is for
  plugins and user scripting only.
- `[ ]` Lua bindings **autogenerated**, not hand written. Today there are ten hand-written functions in
  `lua_engine.c:194-205` and the `nya` global is assembled from a literal, which is why the editor reports
  "Undefined global `nya`". Generate the bindings and a definitions file from reflection.
- `[ ]` Namespacing that survives a large plugin collection, so we do not end up where Minecraft did.
- `[ ]` Per-plugin tracing: time per frame and memory held, per plugin.
- `[ ]` Plugin errors reported to the plugin's own developer, and visibly distinct from engine errors.
- `[ ]` A permission system the game fixes **once, at compile time**: plugins may show UI but not bind keys, or
  may do everything, or are sandboxed away from the filesystem and the network.
- `[ ]` Users can enable and disable engine systems and load their own assets. Depends on the system registry.

## `[ ]` Distribution

Modes, restated so they stop drifting:

| Mode | Meaning |
| :--- | :--- |
| `debug` | Something is wrong in the code; find it. Sanitizers on. |
| `dev` | Ordinary development. Sanitizers off. |
| `release` | What users get. Optimized. |

`debug` and `dev` both use filesystem assets with hot reload, and both produce perf data. `release` has
submodes: plain release is native, `steam` is release through Steam, and flatpak, pacman, AUR, scoop and nix are
the packager ones.

- `[ ]` `dist/` with a folder per target: linux, windows, steam-linux, steam-windows, linux-pacman,
  linux-nixos, web, plus the Lua bindings API. **Binary releases only — a user never compiles from source.**
- `[ ]` A distribution contains: the executable; packager-specific files (manifests, desktop entry, Steam
  library, man page, licence); an optional `assets/` if not bundled into the executable; a `data/` folder
  holding user-editable settings and colour theme plus non-editable save data; and `plugins/`.
- `[ ]` Assets on the filesystem are encrypted or obfuscated so they cannot be extracted or modified.
- `[ ]` Settings must be user-editable, so bad settings need good errors naming the key, the value and what was
  expected. Save data is the opposite and gets an integrity check.
- `[ ]` `CHANGELOG.md`, generated from history, shipped with every release.
- `[ ]` `secrets/` committed to GitHub, encrypted with sops and gpg, holding the signing key among other things.
- `[ ]` CI/CD produces every release build so Steam and the packagers can pick up a new version.

## `[ ]` Reported bugs

- `[x]` Resizing the window broke the UI and the fonts. `_nya_ui_scale_derive` derived the scale from the
  window's height, `_nya_ui_look_build` baked fonts at `size * scale`, and the glyph atlas was keyed
  `path@points` with capacity 8 and eviction `REFUSE`, so each scale step minted a key until all 8 were gone
  and text drew blank. On a HiDPI display that happened at startup with no resize: 6813 "no free glyph atlas
  slot" warnings in a 25 second run, for `Aldrich.ttf@19` and `@16`. Fixed three ways: the scale is 1 unless a
  player sets one and never follows the window, the cache evicts least recently used after flushing the batch
  that may still name the texture, and a refusal reports once per handle. The title screen now bakes two
  atlases where it baked seven, and warns nothing.
- `[x]` The UI must not autoscale with screen size. `NYA_UIStyle.scale` is the only thing that moves it, with
  `follow_display_scale` as an opt-in for HiDPI.
- `[x]` Log spam, including things logged as errors that are not. Two causes, both gone: the atlas warning
  above logged per frame rather than per handle, and `nya_app_init_with_options` logged "No supported
  SDL_GPU backend found" as an ERROR where it was the expected state. A subsystem is now `optional` and an
  unavailable one is reported at debug level, which also means a dedicated server starts on a box with no
  GPU at all — it could not before. A 20 second run of the title screen now logs one line total, the
  Wayland icon warning, once per process as it should.
- `[x]` Shadows moved laggily. The light basis snapped elevation and azimuth to 0.5° steps, which halved
  the pixels changing per frame by freezing most of them: over 240 frames at 60 fps with the two minute
  day, 197 of 239 frames were frozen and the worst single frame jumped 4.678 texels. Following the sun
  exactly: 0 frozen frames, worst jump 1.481.
- `[x]` The fire flickered. A draw went to the opaque stream whenever its colour was fully opaque, and a
  particle is born at exactly alpha one, so for its first tick every flame particle drew through the
  opaque pipeline: depth written, no addition, a solid square punched through the plume. Blend mode is
  explicit caller intent and alpha is a heuristic, so additive now never counts as opaque.
- `[ ]` Two tests are flaky under a loaded parallel suite run and pass 10 to 12 of 12 standalone. Both are
  wall clock dependent under the sanitizers, and a flaky test is a bug with priority, so neither should sit
  here long.
  - `test_robots`, on `the drones move`. Not state: it isolates its own save root. Under load the training
    job gets through fewer generations, and a genome that has not evolved far can legitimately hold every
    drone still, which the test reads as not moving. Give it a deterministic generation count rather than a
    wall clock budget.
  - `test_trace`, on the `spin_ns` timing windows. Same shape, and it predates the system registry: the
    registry's own accounting is off by default and deliberately reads the real monotonic clock rather than
    the simulated one, because a simulated clock advances one tick per frame and would report every system
    as costing the same.
- `[ ]` RenderDoc closes immediately instead of capturing. Not the anti-tamper check — that early-returns
  unless `NYA_SHIPPING_BUILD` (`base_integrity.c:148,172`). Cause still unknown.
- `[x]` `monocypher.h` not found, `NYA_LuaVM` unknown, `windows.h` not found, and the
  `modernize-redundant-void-arg` lint were all `.clangd` gaps. Fixed.
- `[x]` `build-steam-linux` red in CI: monocypher was missing from the steamrt vendor set. Fixed.
- `[x]` `test-windows` red on `test_replica`: a 624 KB `NYA_NetReplicaMap` on a 1 MB Windows stack. Fixed.

## `[~]` Crash reporting

- `[x]` On an assertion, a window showing the assertion and the log leading up to it, with Close, Copy and
  Send to developer. It is an SDL window of its own using the 2D renderer and SDL's built in font, not the
  engine UI: the thing most likely to have crashed is the GPU device or the code feeding it, and a reporter
  that needs six subsystems alive cannot report the crash that took one of them down. See `debug_crash.h`.
- `[x]` The report carries build info, platform info (CPU, RAM, VRAM), the log ring and the error with its
  stack. Build metadata comes from `nya_build_info` so the report, the menu corner and the log agree.
- `[x]` Send to developer writes a file under the log directory and names the path.
- `[ ]` A transport behind `nya_crash_report_submit`. Deliberately deferred: no endpoint chosen yet.
- `[ ]` A window on the hardware fault path. A fault arrives in a signal handler on the faulting thread, and
  calling SDL from there can deadlock against a lock that thread already holds, so a fault writes the file
  and names it on stderr instead.

## `[~]` UI

- `[x]` Not two files. `ui.c` and `ui.h` became `ui.c` (lifetime and the one static state), `ui_layout.c`,
  `ui_style.c`, `ui_input.c`, `ui_draw.c`, `ui_widgets.c`, `ui_text.c` and `ui_internal.h`.
- `[x]` Text input editing: selection, shift+arrows, ctrl+word, ctrl+A, copy, cut, paste, ctrl+backspace,
  double click word and drag select. The header's old "selection and clipboard rejected" rationale is gone.
- `[x]` Dropdowns, radio buttons, tabs, draggable panels, tables, line and bar charts, icons, subtree
  opacity, click bounce, horizontal scrolling and clipping.
- `[ ]` The dropdown does not float. One immediate pass has no z-order, and floating would mean holding the
  caller's `options` pointer past the call that supplied it, so the open list takes room in the layout.
- `[ ]` Icons exist in the engine but nothing in gnyame draws one: the menu sheet has no icon regions, and
  inventing some was not worth it. Compiled, not run.
- `[ ]` A node editor.
- `[ ]` SVG buttons.
- `[ ]` A large text editor widget for writing code in-game, with treesitter syntax highlighting. Needed for
  in-game scripting, and again for the ruey rewrite.
- `[ ]` Transitions between screens.

## `[ ]` Renderer

- `[x]` A flag for every feature: 37 switches per window in `render_features.h`, fed from
  `engine.renderer.features` and hot reloaded. Frustum culling, backface culling, draw sorting and the depth
  test had no toggle before. Tri-state rather than `b8`, so a zeroed struct overrides nothing and no name
  carries a negation. Nothing in it is backend specific, which is what a terminal or web backend needs.
- `[ ]` Reflections still do not exist; the switch is there and answers off.
- `[~]` 2D and 3D fluids, Navier-Stokes. In progress.
- `[ ]` Better 2D and 3D skyboxes. Fog in specific regions rather than only globally, rain, clouds, stars.
- `[ ]` Placeholders for missing assets: log a warning once, then draw something obviously wrong rather than
  nothing. Today a failed asset silently draws nothing (`render2d.c:853`).
- `[ ]` Character customization: recolour, retexture and paint a loaded default model. Clothes and hair later.

## `[ ]` Engine

- `[x]` One system registry for engine and game alike. `core_app.c`'s hardcoded per-frame call lists are gone:
  frame, tick and render phases run from the registry in the same order they ran in before. Systems register,
  unregister, enable and disable at runtime, and a mutation issued from inside a running phase queues and
  applies when that run ends. Every entry carries an owner (engine, game or a named plugin) with per-owner
  time and memory, which is what the plugin system will key off. The overlay's systems page toggles one by
  hand, so `physics2d` off is a freeze frame with everything else still running.
- `[x]` Registry entries are callback handles, not raw function pointers, so a system registered from the
  hot-reloaded game library runs the generation that is loaded rather than the one that registered it. The
  three names are copied into the registry too, since a string literal also lives in the image being
  replaced. The callback registry stopped being a system of its own for this: every entry resolves through
  it, so it comes up before the registrations and goes down after the last `deinit`. `NYA_INTERNAL_CALLBACK`
  is internal where nothing reloads and visible where something does, because dlsym finds neither a static
  nor a hidden symbol. `test_system_reload.c` replaces a callback the way main.c does after a reload and
  checks the registry runs the new one.
- `[ ]` Fast-forward: run the simulation far faster than real time, so a DQN agent playing the game covers far
  more ground than a human would.
- `[ ]` DQN and NEAT driving the real application as a user, to find emergent behaviour and to find crashes.
- `[x]` Scene and settings persistence (`core_scene.h`, reflection driven), for save files and the editor.
- `[x]` Collision layers, named, for both solvers, through Box2D's and Box3D's own filters rather than a
  callback.
- `[x]` Reflection-driven parsing to and from objects and the `nya` format, with the engine's own types
  described too (`reflection_engine.c`), used by scenes and settings rather than only by config.
- `[ ]` Reflection data given at least token protection against reverse engineering. Today every struct and
  field name sits in `.rodata` verbatim.
- `[ ]` Use the config system more, starting with gnyame's `constants.h`.
- `[~]` A better debug UI. A systems page landed; the rest is open.
- `[x]` The main menu shows build kind, commit hash, build time and version in the bottom left corner, from
  `nya_build_info`, which the crash report and the startup log read too so they cannot disagree.

## `[ ]` Steam

- `[x]` Lobbies, peer to peer, achievements, stats and Cloud. `net_steam.c` is a real transport now.
- `[ ]` None of it has been exercised against a running Steam client; it is tested against a fake.
- Note: `plugins/steam/steam.c` **is** compiled and linked for the steam targets. The "Steam is dead code"
  section further down predates that and is stale.

## `[ ]` Discord

- `[x]` Rich presence wired to real game state, through the `core_social.h` facade over Discord and Steam.
- `[x]` Invites: `ACTIVITY_JOIN` and `ACTIVITY_JOIN_REQUEST` arrive as engine events, and a join secret is
  the launch config on the wire. `[ ]` Untested against a running Discord client.

## `[ ]` Testing

- `[x]` Fuzzing through AFL++, `./build run fuzz <target>`, corpus committed. AFL++ is probed, not vendored:
  without it every target still replays its corpus through `./build run test`.
- `[x]` Deterministic simulation testing: `src/nyangine/testing/`, atomic actions and faults composed from
  one seed, simulated clock, assertions as the oracle, failing seeds committed in `simulation_seeds.txt`.
  Every draw is `siphash(seed, step, draw_index)` rather than a stateful generator, so adding a draw inside
  one action does not invalidate every seed recorded before it.
- `[x]` Property tests with a shrinker (`testing_property.h`), covering the round trips.

## `[ ]` Docs and examples

- `[x]` `docs/CHEATSHEET.md`, generated from the headers by `src/build/pp/cheatsheet.c`, so it cannot drift.
- `[x]` `AGENTS.md` at the root pointing at the cheatsheet.
- `[~]` More examples beside hello_world. Landed: `cli_tool` (no window), `plugin_scripting` (the Lua
  surface as it is today), `tui_dashboard` (text only, waiting on a real terminal backend), `net_echo`
  (server and client over the UDP transport). `[ ]` Still to do: multiplayer 2D pong, 3D pinball, and a
  server plus client web app once the HTTP server exists.
- `[ ]` Make the 3D example nicer, and give it the graphics settings menu it currently lacks.

## `[?]` Nyangine as a dependency

Open question, deliberately unanswered for now. When a project uses nyangine and the engine changes, how do the
patches move across? A submodule does not obviously work, because the engine and the game are deeply
integrated — and splitting them is not wanted. Decision deferred until there is a second real project.

## `[⏭]` Android

Out of scope. Nothing android-related is planned or listed.

---

# Open

## `[~]` Stylized renderer

The goal is a stylized, flat coloured look, not a cartoon. Defaults stay restrained: faceted flat colour with
smooth wrapped lighting (materials at high roughness fade the bands into a gradient), a cool tint in shade,
soft ambient occlusion (`softness` spans the band into a gradient), a gentle grade (`vivid.cube` at 0.4),
bloom on lights only, scuff marks and blob shadows as the only decals. Ink outlines, banded shading, speed
lines, tilt shift and the rest stay as options.

Every feature is an options struct set per window, fed from `engine.renderer` in the config, and costs
nothing when off.

- Grading through a `.cube` LUT (`NYA_ASSET_TYPE_LUT`, an RGBA8 3D texture), after ink, occlusion and FXAA
  and before bloom. `vivid.cube` (was `toon.cube`) saturates muted colours and keeps black and white exact; mean saturation in
  the 3D demo 49 to 63, about 0.35 ms at 1280x720. Key 4 in both scenes, `grade_lut` and `grade_strength`
  in the config.
- A cool shade colour gathers in cast shadow and the dark band; sky and ground ambient colours replace flat
  `ambient`; highlight and rim soften over exactly one pixel.
- Screen-space ink from depth and normal discontinuities replaces the inverted hull: silhouettes, hard
  creases past 40°, thinning with distance and fading into fog. Banded ambient occlusion at half resolution,
  FXAA, and debug views (normals, depth, occlusion, ink, cascades). The 3D pass writes an RGBA16F normal and
  distance buffer as a second colour target, only into render textures created with `.normals`; pipelines
  build that variant on first use. Keys 1, 2, 3 and v in the 3D demo, fed from `engine.renderer` in the
  config. Release 1280x720: +0.11 ms and +39.5 MiB at 4x MSAA, +0.06 ms and +11.4 MiB at 1x.
- Ink silhouettes predict the far pixel from the near side's distance slope and check back from two samples
  past the edge; creases must still turn against the faces beyond, so slivers draw nothing. The dotted cube
  edge and valley dashes are gone. `[ ]` Faint short dashes remain on terraces descending away from the
  camera, which are real sub-pixel ledges; a distance-relative floor removed them but cut cube silhouettes.
- Ambient occlusion reaches at least `min_radius` pixels (32). `[ ]` At the default orbit the band is still
  subtle; that is strength and band threshold, not radius.
- Bloom is a built-in half resolution pass (`NYA_PostBloom`, `engine.renderer.bloom`) sharing the depth of
  field target: 0.095 to 0.056 ms, +0.88 MiB while depth of field is off.
- `[ ]` A smaller normal buffer: view space normal only, or sample the multisampled buffer where backends
  allow and skip the resolve.
- Depth of field (tilt shift, or distance focus read from the normal buffer's distance channel), speed lines
  scaled by camera speed, projected box decals in one batched pass, and HDR output (extended linear or HDR10,
  every pipeline still built for the SDR format). Keys 5 to 8. Post order: occlusion, ink, depth of field,
  FXAA, caller passes (grade, bloom, pause grey), speed lines, debug view. Release, 1280x720, 4x: decals
  +0.06 ms and +0.5 MiB, distance focus +0.05 ms and +0.9 MiB, speed lines +0.02 ms, HDR +0.02 ms and
  +3.5 MiB; off costs nothing.
- HDR lifts only the scene: `nya_render_output_scene_end` zeroes the frame's alpha before the HUD draws.
  `[ ]` HDR10/PQ untested on hardware.
- Speed lines converge on the vanishing point of the camera's velocity (`NYA_PostSpeedLines.motion`), centred
  when backing away or moving across the view.
- `[ ]` Distance focus alone pays for the whole normal buffer.
- A skinned, animated bar (bender.fbx) in the 3D demo, lit and shadow casting, posed once per tick so
  every cascade matches the camera; `f` freezes it. The 2D ledge marker is an animated sprite with a frame
  event. `game.animation_speed` sets both clocks live.
- `[ ]` The skinned draw sets no bounds, so it is never culled, and ignores material parts and textures.
- `[ ]` No test reaches the non-headless skinned draw.

Next:

- The 3D scene between `nya_render3d_begin` and `_end` is recorded once as segments of shared state, uploaded
  in one copy pass, and drawn by each shadow cascade and the camera from the same buffers with per-pass index
  lists (a draw keeps a mask of the passes whose frustum it touches). `nya_render3d_shadow_set` replaces the
  cascade loop. Release 3D demo, 1280x720, 4x: uncapped frame 1.00 to 0.41 ms, CPU work at the cap 0.28 to
  0.19 ms, uploads 61 (203 KiB) to 12 (152 KiB), render passes 59 to 25. The shadow atlas exists only while a
  scene casts shadows (-12.6 MB off). `-mf16c` inlines half float conversion. The overlay shows draws, passes
  and upload bytes per frame.
- `[ ]` A scene past the vertex, segment, instance or group ceilings is drawn early in pieces, and earlier
  pieces miss later casters' shadows. Cascades use the light in effect when the scene first draws;
  orthographic cameras cast no shadows.
- `[ ]` Decal grids miss their cache every frame while marks shrink (probe 2.2%, staging 1.1% of samples).
- Indirect draws are not worth it yet: about 15 calls per pass, each with its own mesh buffer or material;
  removing them saved at most 0.04 ms.
- A pipeline cache on disk is not possible through SDL 3.5 (Vulkan passes a null cache; D3D12 and Metal have no
  equivalent exposed). Cold driver cache: 46 pipelines take 222 ms, 11 lazy variants 57.5 ms after the first
  frame; warm, 0.8 ms. SDL does not document pipeline creation as thread safe.
- A render graph is not planned: the pass order is fixed and short, and a graph would be more code than
  the passes it orders.

## `[~]` Fluid volumes

Incompressible Navier-Stokes on a grid (Stable Fluids: advect, diffuse, project), for smoke, fire and
shallow water. Velocity, density and temperature, with buoyancy, weight, vorticity confinement and
box obstacles. `render_fluid.h` carries the reasoning; the short version is below.

- Grid rather than SPH particles. A grid is allocated once and every step touches the same cells,
  where SPH rebuilds a neighbour structure whose cost follows how the particles clump. A grid sweep is
  a fixed loop in a fixed order, where an SPH neighbour list is gathered in whatever order the
  particles currently sit in and a different summation order is a different sum. Semi-Lagrangian
  advection is unconditionally stable, where SPH is CFL limited and one fast particle demands
  unbounded substeps. And volumetric smoke out of SPH means splatting into a grid to draw it anyway.
  What would bring SPH back is a small volume of liquid in a large empty space, a wake across a lake,
  and that is a second module rather than a rewrite of this one.
- One solver, and it is three dimensional. A 2D volume is a grid one cell deep whose two z boundary
  planes mirror the interior, and every term collapses by itself: the seven point Laplacian's z
  neighbours both equal the centre cell, so `p_l + p_r + p_u + p_d + 2p - div = 6p` is exactly the 2D
  equation, and the 3D curl with `w = 0` is the 2D scalar curl. The equality is bit for bit and a test
  asserts it, which is why the third axis is flattened once in `nya_fluid_create` and `nya_fluid_emit`
  rather than branched on per cell: a ternary inside a loop lets the compiler contract the two
  branches differently and the two spaces then disagree in the last bits.
- `NYA_FLUID_PRESSURE_ITERATIONS` is 20. On the 32x48x32 bench grid with confinement off, divergence
  entering a step is 2.09 and leaving it 2.36 at 4 sweeps, 1.39 at 8, 0.93 at 20, 0.87 at 40 and 0.89
  at 80. Twenty is where another sweep stops changing the picture.
- Drawing is off until a window asks. `NYA_FluidRenderOptions` sits on the window beside the post
  chain's scene features and is zeroed, so `nya_fluid_draw` returns before it reads the grid. 2D draws
  one bilinearly shaded quad per live cell through render2d, 3D additive camera-facing splats through
  render3d, which the post chain composites and bloom picks up.
- Volumes register as the `fluid_volumes` ceiling and their bytes as the `fluid_cells` gauge, both
  lazily on the first create, so a program with no fluid in it registers nothing.
- The solver draws no randomness, reads no clock and uses no threads, so `nya_fluid_checksum` is an
  oracle: `tests/nyangine/renderer/test_fluid.c` drives two volumes from the simulation harness and
  asserts the same seed replays the same run.
- gnyame: a 48x32 steam vent over the 2D tilemap, and a 12x18x12 column over the 3D bonfire whose
  obstacles are the pile's crates. `9` toggles the drawing in either scene.

- `[ ]` The step is the cost, not the draw: 40 Gauss-Seidel sweeps a step (20 per projection, two
  projections) over the whole grid, single threaded and serially dependent along x so it does not
  vectorize. Red-black ordering would vectorize it and keep the convergence; a multigrid V-cycle would
  beat both and is a lot more code. Measure before choosing.
- `[ ]` Dropping the first projection, the one before advection, halves the solve. Stam keeps it so
  advection rides a divergence-free field; nobody has measured what it is worth here.
- `[ ]` A raymarched 3D volume instead of splats. It needs a 3D texture uploaded every frame, a
  pipeline and a shader per backend, so it waits for compute passes.
- `[ ]` No obstacle comes from a collider's actual shape, only from an axis-aligned box the caller
  passes. A rotated crate is still a box to the fluid.

## `[~]` Networking

UDP is always encrypted, loopback never: a stateless handshake (padded CONNECT, 45 byte cookie challenge, no
amplification) authenticated with X25519 against the server's long-term key and an optional player key, then
XChaCha20-Poly1305 per packet with a key per direction and a 32 packet replay window. `--server-key` pins the
server; `nya_net_key_pair_load` keeps its identity. Handshakes are rate limited per address (8/s, burst 16) and
capped at 4 connections per IP. The server applies one command per tick (burst 4), checks command ticks, caps
speed with `max_speed` and kicks on a decaying violation score. Snapshots carry only changed entities in fixed
point against the acknowledged baseline, rotations smallest-three in 32 bits. Clients reconcile against the
command the server confirmed, follow the server's tick rate and draw replicas with a jitter-adaptive delay and
100 ms extrapolation. `--net-latency/-jitter/-loss/-duplicate/-reorder` condition the transport. Every wire
decoder is fuzzed from a fixed seed. Bench, 48 crates and 6 drones: 44.3 to 6.9 kB/s down settled, 10.3 to 3.9
up. Under 120 ms, 20 ms jitter and 5% loss, prediction converges with no corrections at about 3 kB/s each way.

- `[ ]` One fragment per datagram; 28 bytes per packet overhead dominates small snapshots.
- `[ ]` Cumulative acks only; lag compensation rewinds to the acknowledged tick, not the render time.
- `[ ]` Version-rejected peers linger until timeout; hostname resolution blocks up to 5 s.
- `[ ]` No allowlist API for player keys; the Steam transport is a stub.

## `[~]` Steam targets and anti-tamper

`steam-linux` and `steam-windows` link the Steamworks SDK and ship its library beside the executable.
`NYA_AppOptions.steam_app_id` relaunches through Steam when started outside it, runs callbacks once a frame and
plays on without a client. steam-linux compiles every vendor and the game with clang 22 against the pinned sniper
SDK sysroot (downloaded into vendor/steamrt, 1.2 GB): GLIBC_2.29 at most, curl on the runtime's GnuTLS. CI builds
and verifies all four targets; CD publishes both depots.

Anti-tamper: executable stamp and chunked code baseline on a startup thread, a sweep of one 64 KiB chunk every
250 ms (13 µs), per blob entry hashes checked on first load, and a watchdog requiring the checks to have run and
agreed. Failure logs one line and exits 86. `base_integrity.h` states the limits: this raises the bar, the server
stays the authority.

- `[ ]` Steam Cloud rules; `net_steam.c`; a real client and depot upload not exercised.
- `[ ]` The steam-linux binary only starts inside the runtime on hosts with a newer nettle.

## `[~]` Audio propagation and effects

Positional voices are traced through a batched ray callback the app wires to Box3D or Box2D: rays over a source's
extent give partial occlusion, a reverse ray gives blocker thickness for transmission, and four probes find a way
around with a Maekawa detour loss that pulls the sound toward the opening. Fourteen listener probes, four a frame,
estimate enclosure and distance, drive the sound bus reverb and place six panned echo taps. One ray budget (64,
ceiling `audio_rays`) is shared round robin, nothing is allocated per frame, nothing is cast when off. Every bus
runs a lock-free chain: high/low pass, 3-band EQ, compressor, echo, echo taps, reverb, limiter, eased per 32
frames. Config under `engine.audio`. Cost: full chain 22.9 µs per 10 ms buffer; 16 hidden voices 5.4 µs a frame.
Behind the basin rim the fire drops 20 dB and its spectral centroid goes from 3238 to 233 Hz.

- `[ ]` Interaural time delay and head shadow need our own stereo panner in place of SDL_mixer's mono 3D path.
- `[ ]` Thickness is the span between first hits, so two thin walls read as one thick one.
- `[ ]` Echo taps do not check that the source sees the surface; one diffraction reach.

## `[~]` Interpolation between ticks

Update runs at a fixed tick and frames draw whenever the display allows, so a frame can land between ticks
or see none. `nya_app_tick_alpha` says where between the last tick and the next it sits. Particles draw from
their previous tick's position and age toward the current one; before, the smoke and sparks froze on frames
without a tick and jumped on the next, which read as flicker (captured: every other frame unchanged).

- Entities capture their transform at the start of each tick (`nya_system_entity_transforms_capture`, 0.7 ns a
  slot) and draw through `nya_entity_render_position` / `_rotation` (4.6 ns). `nya_entity_transform_snap` makes
  a jump draw instantly and carries children; spawn, teleports, zero duration `move_to` and net replicas snap.
  `NYA_Entity` is 704 bytes (was 672). The 2D camera interpolates; the skeleton animator samples the clip
  between ticks (`nya_skeleton_animator_render_pose`). At 120 fps against a 62.5 Hz tick the 3D cube region's
  per-frame change evenness went from 0.87 to 0.16 (CV), the 2D region from 0.90 to 0.04.
- Fixed on the way: 3D bodies integrated their mirrored velocity a second time and drew a tick ahead.
- `[ ]` `NYA_SkeletonPlayer` (layers, crossfades, root motion) still samples per tick; y-sorting uses the
  current tick; the frame rate cap is not in the config.

## `[~]` Shadows

Cascades are fitted to slices of the camera frustum by bounding sphere. The crossfade is verified on screen.
The light basis snaps to 0.5° steps, which halved the pixels changing per frame under a still camera. Taps
filter bilinearly and the edge is cut crisp. Cascade count, map size and shade colour are per window
(`NYA_Render3DShadowOptions`), fed live from the config with `shadow_bias`. Two cascades by default: the
third cost 0.3 ms with no visible gain.

- `[ ]` Cascade selection by depth switches hard, and casters more than two extents toward the light from a
  cascade's centre are clipped out of it. Neither shows in the demo.
- `[ ]` A faint checker on steep terrain walls under the water.
- `[ ]` Ink draws dashed streaks along the steep terrain slope and dotted along a cube's bottom edge in the
  close 3D shot.
- Point and spot light shadows are not planned: six scene passes per light.

## `[~]` Ceiling auditing: HUD done, config over macros blocked

Ceilings are registered and shown in `debug_overlay.c`, fullest first, amber past 75%, red past 90%. There
are 33 registration sites now, up from 20, as the IPC, control, WebSocket, simulation, registry and UI
tables came in.

- Blocked: `NYA_TWEEN_MAX`, `NYA_ENTITY_MAX` and `NYA_RENDER2D_FONT_CACHE_MAX` cannot become config,
  because `NYA_CONFIG` is a global in the game DLL (`gnyame/config.h`) that no engine module can read.
  The same is why `shadow_bias`, `shadow_cascades` and `shadow_map_size` are loaded and read by nothing.
  Either the engine owns the config instance or these stay macros.

## `[~]` SDF text

The menu draws through the NYA_Font registry ("menu" at 22, "menu_title" at 44 as a distance field).
Compared on screen with the bitmap: at a smoothing floor of 1/16 the edge was two pixels and softer; at 1/32
edge and stroke weight match.

- A distance field line measures as wide as the same face in coverage (SDL_ttf added the spread); the title
  sits where the bitmap did, and the 28 pt title matches the bitmap within 0.8 px.
- The atlas latches its mode at bake time.

## `[ ]` Budgets

3D scene, release, 1280x720, 4x MSAA: 116 MB RSS, about 300 MB VRAM.

### Glyph atlas

Atlases are R8 coverage with `NYA_RENDER2D_PIPELINE_TEXT` for coverage text, 128 cells each (was 512). The
busiest atlas fills 53 (game HUD plus debug overlay), the menu's `@22` 28. A full atlas warns once and draws
new glyphs blank. Distance field cells include SDL_ttf's 8 texel spread on each side, which clipped 68 of 95
glyphs at 17 pt before; the `@44` distance field title atlas is 1440x544.

`NYA_RENDER2D_FONT_CACHE_MAX` holds 24 atlases and evicts least recently used, after flushing the batch, since
a queued vertex can still name the texture being released. That number is a memory budget, about 90 KB per
atlas at body size, not a correctness bound: a miss costs one rebake, not blank text for the rest of the run.

Glyphs upload one cell at a time through a cell sized transfer buffer: the menu and HUD fonts staged 3.4 MB
of transfer buffers, now 7 KB.

### The scene is recorded once a frame

Two cascades plus the camera pass by default, all drawing one upload. Before that change:

| Symbol                        | Share | Note                           |
| :---------------------------- | ----: | :----------------------------- |
| `VULKAN_UploadToBuffer`       |  2.4% | four uploads of the same data  |
| `_nya_render2d_quad`          |  1.6% |                                |
| `nya_render2d_text_with_font` |  1.2% | shaping per string per frame   |
| `nya_render3d_quad`           |  1.2% |                                |

Done: `nya_render3d_sphere` draws a registered unit sphere, the shadow pass no longer sorts, terrain
physics is a heightfield.

- A depth-only shadow vertex format was tried and reverted after it broke shadows. Look at the vertex
  layout the shadow pipeline is built with before retrying.

### Vertex formats

`NYA_Vertex3D` is 36 bytes (was 64): FLOAT3 position and normal, HALF2 uv, HALF4 colour. Colour stays
half float because emissive colours exceed one. `NYA_VertexSkinned3D` is 44 bytes (was 96): the same plus
UBYTE4 bone indices and UBYTE4_NORM weights summing to exactly 255, the rounding remainder on the strongest
influence. bender.fbx's vertex buffer went from 621 KB to 285 KB.

- `[ ]` Octahedral SNORM16x2 normals would reach 28 bytes, but `mesh3d_edge` takes `fwidth` of the
  interpolated normal. Measure first.

### Asset blob

Entries are LZ4 compressed when that saves `NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES`, expanded once
and shared by reference count.

- `[ ]` Bake less: nothing checks whether an asset is ever loaded.

### Binary size

Linux release 21.5 to 11.5 MB, Windows 23.6 to 10.8 MB, with every plugin still built in:

- Vendor options drop what the engine never calls: SDL's 2D renderer, GL, camera, haptic, dialog, tray,
  KMSDRM and software blitters; WebP and libpng (SDL_image's stb_image decodes PNG and JPEG); plutosvg;
  libgme, libxmp, WavPack, MIDI and the external Vorbis/FLAC/MP3 libraries (SDL_mixer's built-in decoders);
  curl features the request plugin does not use. -5.7 MB Linux, -7.3 MB Windows.
- Vendors build at -O2 with a section per function, collected in the release link. Frame times and benches
  did not move; the engine stays -O3, 4.4% faster than -O2 on the benches.
- A Windows release exe exports nothing (-1 MB), and Windows vendors cross compile with clang, since mingw
  gcc's sections are not COMDAT and a PE link keeps them.
- No local symbols (Linux) or COFF symbol table (Windows); traces are identical without them.
- The blob bakes only the shader formats a target loads and leaves out `assets/icons`
  (`NYA_ASSET_UNUSED_DIRECTORY`): 1525 entries to 142.
- `.eh_frame` stays (0.74 MB): libbacktrace and LuaJIT unwind through vendor frames with it.
- `[ ]` SDL's dynamic API off would save 0.66 MB but stops a Steam runtime swapping SDL.
- `[ ]` harfbuzz is 1.2 MB; its HB_LEAN config, if shaping quality holds.
- After pulling, delete the vendor build trees so they reconfigure; CI rekeys its cache on the recipes.

### Static memory

From `nm --size-sort -S` on the release binary. `.bss` went from 2.45 MB to 0.77 MB.

| Object                                | Size         |                                          |
| :------------------------------------ | -----------: | :--------------------------------------- |
| `NYA_ASSET_BLOB`                      | 2.4 MB       | `.rodata`, LZ4 per entry                 |
| `b3_worlds` / `b2_worlds`             | 37 + 21 KB   | 8 worlds each via the vendor rules (was 596 + 344 KB) |
| `_nya_audio_system`                   | 3.6 KB       | reverb lines allocated per bus on first use (was 446 KB) |
| `_NYA_NET_CLIENT` / `_NYA_NET_SERVER` | 4 + 3 KB     | replica map only for remote clients, peers per used slot |
| `dphaseTable` / `tllTable`            | 256 + 128 KB | vendored audio decoder tables            |

Release RSS: 2D game 72.2 to 66.8 MiB, 3D demo 77.9 to 72.5 MiB. The entity table reserves address space and
commits 256 slots (172 KB) at a time as spawning reaches them, instead of 5.25 MiB zeroed at startup; spawn
plus despawn of 1024 went from 147 µs to 26 µs, mostly because `nya_tween_cancel_target` stops at the last
live tween. A checkout built before this keeps 128 world box2d/box3d until `vendor/box2d/build-*` and
`vendor/box3d/build-*` are deleted.

### VRAM

MSAA is runtime configurable (`NYA_RenderOptions.msaa_samples`, `engine.renderer.msaa_samples` in
`engine.nya`, hot reloaded). Pipelines build single sampled and multisampled variants on first use. The post
chain holds its second target, single sampled without depth, only while two or more passes run.

`gpu_textures` at 1280x720, debug, by sample count 1 / 2 / 4 / 8:

| Scene    |    1 |    2 |    4 |     8 |
| :------- | ---: | ---: | ---: | ----: |
| 2D game  |  7.1 | 24.7 | 45.8 |  88.0 |
| 3D demo  | 40.2 | 61.3 | 89.4 | 145.6 |

At 4x this was 77.5 and 107.0 MiB before. The debug overlay's `gpu_textures`, `gpu_buffers` and
`gpu_transfer` rows count what SDL is asked for, not the driver's pages.

- `[ ]` Half resolution bloom target.
- `[ ]` Measure driver VRAM, not only what SDL is asked for.

### Fluid volumes

Release, -O2, one whole step at 20 pressure sweeps: forces, confinement, two projections, four
advections and the scalar pass. Held is `nya_fluid_memory_bytes`, thirteen f32 fields plus a byte of
obstacle mask per cell; resident is the volume's arena. Allocated once at `nya_fluid_create`, and a
step allocates nothing.

| Grid        |  Cells | ms/step | MiB held | MiB resident |
| :---------- | -----: | ------: | -------: | -----------: |
| 2D 48x32    |   5100 |   0.300 |     0.26 |         0.26 |
| 2D 64x48    |   9900 |   0.612 |     0.50 |         0.51 |
| 2D 128x96   |  38220 |   2.598 |     1.93 |         1.95 |
| 2D 192x144  |  84972 |   5.946 |     4.30 |         4.33 |
| 3D 12x18x12 |   3920 |   0.333 |     0.20 |         0.21 |
| 3D 16x24x16 |   8424 |   0.875 |     0.43 |         0.43 |
| 3D 32x48x32 |  57800 |   8.089 |     2.92 |         2.95 |
| 3D 48x48x48 | 125000 |  20.220 |     6.32 |         6.37 |
| 3D 64x64x64 | 287496 |  50.197 |    14.53 |        14.55 |

Linear in cells, so the grid is the whole cost decision. The sweep count is the other half: the same
32x48x32 grid is 2.75 ms at 4 sweeps, 4.09 at 8, 8.09 at 20, 14.75 at 40 and 28.10 at 80. gnyame runs
48x32 in 2D and 12x18x12 in 3D, which is 0.30 and 0.33 ms a tick and what a demo can pay beside
everything else in the scene. `bench/bench_fluid.c` is where all of this comes from.

- `[ ]` Drawing is not in these numbers. A 2D volume is one quad per live cell into the existing
  batch, a 3D one a splat per live cell into the mesh batch; neither has been measured against a
  frame.

### Text shaping

`nya_text_shape_with_font` and `nya_text_measure_with_font` keep one `TTF_Text` per (wrap width, face and
size, text) in a least recently used `NYA_Cache` (`text_runs`, 128 entries, keys up to 256 bytes), tagged
with the font asset's generation. A 20 line HUD frame went from 41.5 µs to 3.4 µs; the main menu's draw from
about 25 µs to 7 µs.

- The distance field request scan runs only after an asset loads (`nya_asset_generation`).

### Integrity check

The runtime baseline hashed `__executable_start` to `etext`, which faults when the linker leaves an unmapped
page before the code segment (a release build crashed at startup). It now hashes the executable `PT_LOAD`
segment from the program headers.

### Measurement

`nya_arena_resident_bytes` measures resident pages per arena. The arena report and the debug overlay show used
and resident per arena plus process RSS; opening the overlay in gnyame logs the full report.

## `[ ]` Reports from other machines

- `[ ]` Windows: the 3D demo drew only the models, particles and sky on one machine (terrain, pile, lamps and
  water missing). Wine with Direct3D 12 and Vulkan both render correctly, so it needs that machine's log:
  the `Render system initialized (<driver>)` line and any `failed to load`.
- `[ ]` "No sound" reported more than once, then "sound is back", then gone again. Recording the game's own
  PipeWire stream on Linux gets audible impacts in the 2D game and the 3D demo, also after a code reload.
  Each process opens two output streams and only the newer one carries sound, so a mixer showing the
  first looks silent. Music starts paused by design (`m`). Needs the platform and log of a silent run.
- `[ ]` Resident memory differs by machine (300 MB, 150 MB, 50 MB + 150 MB VRAM). Arenas no longer allocate
  64 MiB regions, which Windows committed up front; the rest is the GPU driver mapping into the process,
  which on integrated GPUs counts texture memory as RAM. Measure per driver before changing anything.

## `[ ]` Startup time

Release on Linux/Wayland reaches the first frame in about 46 ms (2D) and 56 ms (3D) after exec (was 135).
Startup logs engine init, subsystems and first frame; each subsystem's bring-up time is at debug level.

- The gamepad subsystem starts after the first frame, so frame two carries its ~42 ms. It cannot move to a
  thread: SDL requires `SDL_InitSubSystem` on the main thread, IOKit binds its HID manager to the calling
  thread's run loop, and the Windows notification windows belong to their creating thread.
- `[ ]` The renderer's GPU device takes ~21 ms and window plus swapchain ~10 ms, both on the main thread.
- `[ ]` Not measured on Windows.

## `[~]` UI system

`src/nyangine/ui` is an immediate-mode module, split by domain: `ui.c` the pass lifetime and the per window
state, `ui_layout.c` containers and placement, `ui_style.c` the look and the scale, `ui_input.c` presses and
focus, `ui_draw.c` the shapes, `ui_widgets.c` the widgets and `ui_text.c` the editable field, with
`ui_internal.h` between them and `ui.h` as the contract. One function runs as an input pass in `on_update` and
a draw pass in `on_render`; presses roll per tick, so each is handled once however many ticks a frame runs. Ids
hash label and panel. Fixed tables (64 widgets per pass, 64 panels) registered as ceilings, no heap. The style
is a struct where zero is the cartoon default, fed from `engine.ui` in the config.

Widgets: label, button, selectable, radio, toggle, slider, one line text input, colour picker, tabs, dropdown,
table, line and bar chart, icon, space and scrim. Containers scroll and clip on both axes, panels can be
dragged by their title, opacity groups fade a whole subtree, and a widget pops on focus and bounces on
activation. The field selects with shift, moves and deletes by word with control, and copies, cuts and pastes
through the system clipboard.

gnyame's menus and both HUDs use it, and the pause screen's widgets panel exercises the rest while plotting
frame times. Release: pause menu draw about 0.02 ms and 20 draw calls, input pass 0.004 ms, binary +41 KB.

- `[ ]` Merge same-state 2D draw ranges after sorting, so a menu is a few draw calls instead of two per widget.
- `[ ]` A dropdown's list takes room in the layout instead of floating over what follows. Ordering it last
  would mean holding the caller's options pointer past the call.
- `[ ]` No multi-line text field, and no navigation into an open dropdown with the keys alone.
- `[ ]` The German key hint line runs past a 1280 wide window.

## `[ ]` Editor

`src/nyangine/editor/editor.c` and `.h` are empty.

## `[~]` Packaging and distribution

`packaging/` holds an AUR `gnyame-bin` package, a Flatpak manifest wrapping the release tarball, winget and
scoop manifests for the portable Windows zip, and SteamPipe scripts with `upload.sh`. CD checks the tag
against `VERSION`, then publishes both binaries, the Linux tarball, the Windows zip, rendered manifests and
`SHA256SUMS`. The Windows exe carries its icon and version info. `packaging/README.md` covers cutting a
release per channel.

- `[ ]` namcap, flatpak-builder and real winget/scoop installs have not been run.
- `[ ]` Nothing submits to the AUR, winget-pkgs or a scoop bucket automatically.
- `[ ]` A C2Y build needs glibc 2.38 (C23 `strtol`/`sscanf` variants, `strlcpy`), and curl links
  `libssl.so.3`. The Steam sniper runtime has neither, so build the Linux depot in the sniper SDK with
  OpenSSL static, or target Steam Linux Runtime 4.0, where the binary already runs.
- `[ ]` A real code signing certificate; the signing hook uses the sample `.pfx`, and an unsigned browser
  download warns under SmartScreen. winget and scoop installs do not.
- Saves and logs go under the app id in the user data directory (`~/.local/share/gnyame`, `logs/` inside),
  not the working directory, which is the install folder on Steam. Settings saved under the old `nyangine`
  directory are not migrated.

## `[~]` Steam

This section used to say Steam was dead code and that `plugins/steam/steam.c` had never been compiled.
Both were already false when it was written: `-DNYA_PLUGIN_STEAM` is on the steam link lines,
`plugins.c` includes `steam.c` under that guard, and the build ships `steam_api64.dll` beside the exe.
Kept as a note because it cost an afternoon to disbelieve.

What is real now: lobbies, peer to peer through `SteamNetworkingMessages`, achievements, stats and Cloud,
with `net_steam.c` a working transport satisfying the same interface as UDP and loopback.
`SteamAPI_RestartAppIfNecessary` runs first, init falls back when no client is running, callbacks run each
frame. No SteamStub DRM wrapper: it rewrites the exe, breaking the integrity CRC and the signature.

- `[ ]` None of it has run against a real Steam client. It is tested against a fake implementation of the
  transport interface, which proves the shape and nothing about Valve's behaviour.
- `[ ]` A real depot upload has not been exercised.
## `[~]` CI

First fully green run on 2026-09-17 (Linux and Windows: vendors, check, tests, builds). A push cancels the
run in progress, so a vendor rebuild cut short saves no cache; wait for the vendor jobs before pushing again.
`actionlint` catches workflow syntax errors locally, which GitHub reports only as a run with no jobs. Vendors now build in one job per platform that saves the cache right after the
build, keyed on submodule revisions, vendor recipes and both toolchain directories; the check, test and
build jobs restore it.

- A Windows host does not build shadercross (DXC does not compile under MinGW). CI compiles shaders in
  the Linux vendor job and passes them on; a Windows developer needs `assets/shader/compiled/` from a
  Linux machine. `[ ]` A prebuilt DXC for Windows would remove that.
- Engine, game and test rules compile to `.objects/` and link in a second rule, launched through ccache
  when it runs (`NYA_CCACHE` overrides or disables). CI caches `$RUNNER_TEMP/ccache` per OS and job
  (`test`, `build`), 500M each. Tests link one engine object compiled once, unless they define anything
  before including the engine or name a `_nya_`/`_NYA_` identifier: 82 of 163 share it. Measured on the
  8 thread dev machine, other builds running beside it:

  | Command                       | Before | Split, no cache | ccache cold | ccache warm |
  | :---------------------------- | -----: | --------------: | ----------: | ----------: |
  | `./build run test`            |  106 s |            73 s |       123 s |        27 s |
  | `./build build debug-linux`   |        |                 |       5.0 s |       1.6 s |
  | `./build build release`       |        |                 |      18.8 s |      12.7 s |

  Warm, the 27 s is about 19 s of running tests one at a time and the links. Release stays slow warm
  because both executables are LTO, and LTO codegen happens in the link.
- Compile commands no longer carry the commit hash, so a new commit gets 170 of 170 direct ccache hits
  and a 27 s warm test run (was preprocessed hits and 42 s).
- The shared engine scan lexes the engine headers and shares the engine when every `_nya_`/`_NYA_` name a
  test uses is declared there outside a `NYA_INTERNAL` line: 139 of 171 tests share it. Cold
  `NYA_CCACHE=off ./build run test` went from 76 s to 45 s. The rest name static internals, define
  something before including the engine, or use generated reflection names.
- `nya_build_parallel` is a pool that starts the next rule as soon as any finishes. `./build run test` on
  the 8 thread dev machine: 111 s with batches, 106 s with the pool. The gain is small because test
  compiles all take 3.5 to 5 s; it grows with uneven rules.
- `[ ]` No clang-format gate: the tree does not satisfy `.clang-format`, and fixing that is a 73k line
  reformat with include regrouping to review.

## `[~]` The verification rule

Six drones in the 2D scene learn to fly to the player: five fly the best NEAT genome, one a DQN. Both train
on one job every 0.25 s and are scored on the same eight test flights; the HUD shows the numbers and the
genome. A nav flow field around terrain and map picks each drone's next point. The best genome is saved to
`robots.nya` and each run is a row in `robots.db` (sqlite). `game.robots` in `engine.nya` toggles and tunes
it live. Release: 3 to 4 µs a tick on the main thread, a 2 to 7 ms job every 0.25 s, about 0.8 MB resident,
nothing when disabled. Walking and all menus work on a gamepad, tested headless through SDL's virtual
joystick.

- Gamepad edges now roll at the end of each update tick like keys; per frame, a frame without a tick lost
  a press. `[ ]` The menus still detect pad presses from held state themselves and could use the edges.
- `[ ]` Nothing in the game calls render occlusion (`nya_occlusion_*`) or mesh LOD (`nya_render3d_lod_*`).
- curl, Discord and Steam stay unwired: each needs a network, a running client or an app id.

---

# Findings

### A content keyed cache costs a hash, and only inlining keeps it near a pointer memo

`NYA_Cache` replaced the asset memo, the glyph atlas table and the mesh registry. `nya_asset_get` over
eight handles went from 4.1 ns (slot by pointer, strcmp) to 4.9 ns. Out of line it was 7.5 ns: the calls
to `nya_cache_get`, the hash and `memcmp` each cost about a nanosecond, and relinking a recency list on
every hit cost more than scanning stamps on the rare evicting insert. FNV-1a would have been pointless
here at 12.8 ns per path; wyhash is 2.5 ns with the `strlen`.

### A collocated projection cannot remove what its own divergence cannot see

The fluid solver's divergence and pressure gradient are both central differences over two cells, while
the pressure Laplacian it inverts spans one. The two do not compose, and what survives is the
checkerboard mode the wide difference is blind to. It reads as an under-converged solve and is not
one: on the 32x48x32 bench grid the residual is 0.93 at 20 sweeps, 0.87 at 40 and 0.89 at 80, a floor
rather than a curve. Vorticity confinement makes it much worse, because its force is a cell-scale
field and most of its divergence lands in exactly that mode: at `vorticity = 1` the floor goes from
1.6% of the fastest speed in the field to 5.5%. It does not accumulate, since advection and any
dissipation at all remove the highest frequency the grid holds within a few steps. The cure is a
staggered MAC grid, which is a different solver.

A residual that falls with the sweep count is a solver that needs more sweeps; one that does not is a
discretisation that does not close. Measure both before adding iterations.

### A pointer compare cannot tell a reload from the same face

The glyph atlas noticed a reloaded font by comparing `TTF_Font*`. A new face allocated at the freed
address can compare equal. `NYA_Asset.generation` is unique across the run, so it is the tag instead.

### The ceiling registry cannot forget a row

A registered counter must outlive the process, which a per-window cache does not. Caches register one
static row per name instead, which reads zero once every cache with that name is destroyed.

### A CI runner ignores SIGPIPE, and children inherit it

GitHub runners start jobs with SIGPIPE ignored, which survives `exec`. `yes | head` in a child then gets
EPIPE instead of dying and prints `yes: standard output: Broken pipe`, the 34 extra bytes
`test_bug_command_pipe_deadlock` saw only on CI. `nya_command_spawn` resets SIGPIPE in the child.

### A union member can overwrite the pointer to its own bytes

`NYA_Asset.as_text`, `as_font` and `as_sound` share storage. A font decoded from `as_text.data` wrote
`as_font.font` over that pointer, so unloading freed null and every font and sound leaked its file bytes
inside an arena. The bytes now live in `NYA_Asset.raw`, outside the union. The mesh loader builds into a
scratch asset for the same reason.

### A seen window indexed by id forgets under reordering

The UDP duplicate filter was a bitmap indexed by `id % 1024` that cleared 64 bits ahead of each mark.
Receiving 5 after 10 cleared 10's mark, so a retransmit of 10 was delivered again, and on the reliable
channel it queued behind a delivery id already past it until the queue filled. The window now slides
behind the newest id.

### A code reload must not unmap what the engine points at

The engine keeps pointers into game DLL data: layer ids, perf timer names, config reflection and
instances, asset handle literals. Reloading dlclosed the old image, so after one reload the main window
handle and layer ids were gone and those pointers dangled. Old images now stay mapped (each load opens
its own copy, `RTLD_LOCAL` on Linux so new code does not bind to an older generation), layer ids and
perf names are copied, and state the game must keep lives in the world rather than DLL globals.

### LuaJIT's Makefile probes the compiler even to clean

It defaults to `gcc`, which MSYS2 CLANG64 does not have, and fails with "Unsupported target
architecture" before doing anything. Pass `CC` on every invocation, including `clean`.

### Render pass state is per pass, and a flush opens a new one

Viewport, scissor and load/store ops belong to an `SDL_GPURenderPass`. A copy pass cannot open inside a
render pass, so any flush that uploads ends the pass and begins another, with per pass state back at
defaults. That caused two shadow bugs: the cascade viewport set once in `nya_render3d_shadow_begin`, and
a `DONT_CARE` depth store that resume then `LOAD`ed. Neither shows in a scene that fits one flush. Set
per pass state in `_nya_render2d_pass_resume`.

### An occlusion buffer belongs to one viewpoint

`nya_occlusion_begin` takes a view-projection. Testing shadow casters against the camera's buffer culls
what the camera cannot see, which is exactly what casts shadows onto visible ground, so shadows blink as
walls pass in front of casters. Frustum planes are rebuilt per pass; anything else viewpoint dependent
in the cull path must be too.

### "Has a shadow pass run" is not "am I in a shadow pass"

`nya_render3d_shadow_active` is true from `shadow_end` to `nya_render3d_end`: false during the first
shadow pass and true for the camera pass. `nya_particles_draw` read it as the second meaning, so
particles went into cascade zero and vanished from the screen. `nya_render3d_shadow_pass_active` answers
the second question.

### A pointer keyed memo cannot memoize a handle built on the stack

`nya_asset_get` memoized on the handle pointer. A handle built in a reused stack buffer has the same
address with different text, so fonts came back at the wrong size and an atlas baked with another
font's SDF flag. The memo now keeps a copy of the handle text. While render_text.c still interned
handles to dodge the bug, two sizes sharing a stack address also evicted each other every frame and put
`nya_siphash` at 5% of the profile; the intern table is gone now that the memo compares text.

### The profile was of the sanitizers

`./build perf` used to read a profile of the `-O0` build with four sanitizers. Profiling release showed
what mattered: `nya_render3d_sphere` tessellating 1152 vertices per marker per pass (3.5%), and menu
text shaping on literals.

### Cascaded shadows are fitted to the frustum

A cascade of constant size a fixed distance ahead only works when the camera is near its subject. Each
piece of the fix is required: slices of the camera frustum so size follows the view; bounding spheres so
texel size does not change as the camera turns and the snap has a fixed grid; and cascade selection by
projecting into each volume, since a cascade is not a sphere around the viewer. Found with three debug
views: shadows off, visibility as greyscale, cascade index as colour.

### What shaping costs

`TTF_CreateText` per string per draw: 43.4 µs for a twenty line HUD, ~71 ns per glyph, 950 ns for one
short line, 700 ns to measure without keeping glyphs. Wrapping costs 6x. See `bench/bench_text.c`.

### Codegen dependencies were missing from half the build

`src/generated/strings.h`, `reflection.c` and `assets.h` come from metarules that only the DLL rules
depended on. Builds worked by ordering until a new shader made the executable compile against a stale
`assets.h`. `nya_build_parallel` builds a shared dependency once per epoch, so every rule names its
codegen.

### Clang vector miscompile (`f128x3`)

`ext_vector_type(3)` over x87 `long double`: clang sizes the stack slot from the packed
`<3 x x86_fp80>` (32 bytes) and stores the padded four lane form (40 bytes). Two and four lanes are
fine. `f128x3` is declared with a fourth lane; `sizeof` was already 64. Reproduced on clang 22.1.8.

### Box2D pre-solve traps

- `enablePreSolveEvents` is per shape and applies to the dynamic body; Box2D ignores it on static shapes.
- Box2D recycles contacts that have not moved and skips the callback, which is exactly a body resting on
  a platform. Recycling is suspended while a drop window is open, via
  `b2World_SetContactRecycleDistance`.

### `TTF_CreateText` with a null engine

A null `TTF_TextEngine` still runs full layout; the engine only draws. That makes shaping and
measurement GPU free. Shaping outputs glyph indices and there is no public codepoint to index mapping,
so atlases are keyed by index and baked lazily.

### LuaJIT allocator on x64

LuaJIT needs its heap in the low 2 GB and ships an mmap allocator for that; custom allocators are
unsupported upstream. The one non-arena allocation in the engine.

### Physics solver order

The solver steps before the entity update, so a velocity set during update is consumed next tick.
Zeroing a kinematic move's velocity on the arrival tick dropped its final step.

### Terrain LOD

- Measure distance to a chunk's bounding sphere, not its centre.
- Bound skirt depth by the terrain's relief, not a multiple of the cell.
- Scale bands by chunk width, not a fraction of the world.

### Unsigned overflow in hashes

Builds use `-fno-sanitize-recover=all`, so a hash whose multiply wraps needs
`__attr_no_sanitize("unsigned-integer-overflow")`. Missed twice, in the kerning memo and the glyph
lookup, and both aborted the first frame that drew text.
