# nyangine: what is left

`[ ]` todo · `[~]` in progress · `[⏭]` deferred

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

# Open

## `[~]` Cartoon renderer

The goal for the 3D renderer is a strong cartoon look. It already has banded wrapped diffuse, a hard
highlight, rim, curvature edge darkening, inverted hull outlines, distance and height fog, four point lights
per draw, instancing, cascaded sun shadows, MSAA and glass.

In progress:

- `[~]` Colour grading through a `.cube` LUT, tuned for clean saturated palettes (see below).
- `[~]` Coloured shadows and a sky/ground ambient instead of flat `ambient`.
- Screen-space ink from depth and normal discontinuities replaces the inverted hull: silhouettes, hard
  creases past 40°, thinning with distance and fading into fog. Banded ambient occlusion at half resolution,
  FXAA, and debug views (normals, depth, occlusion, ink, cascades). The 3D pass writes an RGBA16F normal and
  distance buffer as a second colour target, only into render textures created with `.normals`; pipelines
  build that variant on first use. Keys 1, 2, 3 and v in the 3D demo, fed from `engine.renderer` in the
  config. Release 1280x720: +0.11 ms and +39.5 MiB at 4x MSAA, +0.06 ms and +11.4 MiB at 1x.
- `[ ]` Short ink dashes on distant grazing terrain folds; scale the silhouette threshold with the slope of
  the distance buffer.
- `[ ]` The occlusion band is subtle at the default orbit distance; consider a screen-space minimum radius.
- `[ ]` A smaller normal buffer: view space normal only, or sample the multisampled buffer where backends
  allow and skip the resolve.
- `[~]` Tilt-shift depth of field, projected decals, HDR swapchain output when the display supports it,
  cartoon speed lines.
- A skinned, animated bar (bender.fbx) in the 3D demo, lit and shadow casting, posed once per tick so
  every cascade matches the camera; `f` freezes it. The 2D ledge marker is an animated sprite with a frame
  event. `game.animation_speed` sets both clocks live.
- `[ ]` The skinned draw sets no bounds, so it is never culled, and ignores material parts and textures.
- `[ ]` No test reaches the non-headless skinned draw.

Not started:

- `[ ]` Point and spot light shadows.
- `[ ]` Indirect draws and GPU culling; build the shadow casters once per frame instead of once per pass.
- `[ ]` Pipeline cache on disk. SDL GPU exposes none, so check what each backend allows first.
- A render graph is not planned: the pass order is fixed and short, and a graph would be more code than
  the passes it orders.

## `[ ]` Colour grading through a LUT

The post chain (`render_post.h`) ships bloom, blur, crt, grayscale and pixelate. `mesh3d_tonemap` is a
per-material display curve, not grading. Nothing samples a colour lookup table.

- A `effect_lut.frag.hlsl` sampling a 3D LUT, or a 2D strip if `SDL_GPU_TEXTURETYPE_3D` support is
  uneven (the reason the shadow map is a colour target, see `mesh3d_shadow.frag.hlsl`).
- The asset system has no LUT type. Load `.cube`, the format colourists hand over.
- It goes after tonemapping and before the bloom composite.
- Decide first whether grading fits a renderer built on flat authored colour. The `mesh3d_tonemap` note
  on why ACES was rejected applies here too.

## `[~]` Shadows: fit rebuilt, crossfade unverified

Cascades are fitted to slices of the camera frustum by bounding sphere (`nya_render3d_shadow_for_camera`,
`NYA_Render3DShadowFit.range`). The pass plumbing bugs that hid the fit are fixed: `nya_render3d_end` had
no shadow pass guard, `_nya_render2d_pass_resume` did not restore the cascade viewport, and the shadow
depth target was `DONT_CARE` while resume `LOAD`ed it.

- `[ ]` Verify the crossfade by eye. The outer 15% of each cascade blends, and the PCF penumbra is
  constant in world units. Neither is confirmed on screen.
- `[ ]` The sun moves. `system_sky.c` turns the light every frame, so the snap grid rotates and edges
  crawl even with a still camera. Snap the light basis to discrete steps.

## `[~]` Ceiling auditing: HUD done, config over macros blocked

20 ceilings are registered and shown in `debug_overlay.c`, fullest first, amber past 75%, red past 90%.

- Blocked: `NYA_TWEEN_MAX`, `NYA_ENTITY_MAX` and `NYA_RENDER2D_FONT_CACHE_MAX` cannot become config,
  because `NYA_CONFIG` is a global in the game DLL (`gnyame/config.h`) that no engine module can read.
  The same is why `shadow_bias`, `shadow_cascades` and `shadow_map_size` are loaded and read by nothing.
  Either the engine owns the config instance or these stay macros.

## `[~]` SDF text

The menu draws through the NYA_Font registry ("menu" at 22, "menu_title" at 44 as a distance field).
Compared on screen with the bitmap: at a smoothing floor of 1/16 the edge was two pixels and softer; at 1/32
edge and stroke weight match.

- `[ ]` The SDF title sits about 3 px left of the bitmap position; the measured width seems to include the
  field's padding.
- `[ ]` The HUD's 28 pt title is not rechecked at the new smoothing.
- The atlas latches its mode at bake time.

## `[ ]` Budgets

3D scene, release, 1280x720, 4x MSAA: 116 MB RSS, about 300 MB VRAM.

### Glyph atlas

Atlases are R8 coverage with `NYA_RENDER2D_PIPELINE_TEXT` for coverage text, 128 cells each (was 512). The
busiest atlas fills 53 (game HUD plus debug overlay), the menu's `@22` 28. A full atlas warns once and draws
new glyphs blank. Distance field cells include SDL_ttf's 8 texel spread on each side, which clipped 68 of 95
glyphs at 17 pt before; the `@44` distance field title atlas is 1440x544.

Glyphs upload one cell at a time through a cell sized transfer buffer: the menu and HUD fonts staged 3.4 MB
of transfer buffers, now 7 KB.

### The scene is emitted four times a frame

Three cascades plus the camera pass, regenerated from scratch each time.

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
- `[ ]` Cascade count multiplies all of the above. Three may be one too many now that the fit follows the
  frustum.

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

### Static memory

From `nm --size-sort -S` on the release binary. `.bss` went from 2.45 MB to 0.77 MB.

| Object                                | Size         |                                          |
| :------------------------------------ | -----------: | :--------------------------------------- |
| `NYA_ASSET_BLOB`                      | 12.4 MB      | `.rodata`                                |
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

### Text shaping

`nya_text_shape_with_font` and `nya_text_measure_with_font` keep one `TTF_Text` per (wrap width, face and
size, text) in a least recently used `NYA_Cache` (`text_runs`, 128 entries, keys up to 256 bytes), tagged
with the font asset's generation. A 20 line HUD frame went from 41.5 µs to 3.4 µs; the main menu's draw from
about 25 µs to 7 µs.

- `[ ]` `_nya_font_sdf_apply_pending` looks up each request's face on every measure and draw (about 1 µs of
  the menu).

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

## `[ ]` UI system

`src/nyangine/ui/ui.c` is one line, `ui.h` is `#pragma once`, and the include is commented out in
`nyangine.c`. The game builds menus by hand in `layers.c`.

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

## `[ ]` Steam is dead code

- `net_steam.c` returns `NYA_ERROR_NOT_SUPPORTED`.
- `plugins/steam/steam.c` has never been compiled: `FLAGS_STEAM_*` in `src/build/flags.h` define
  `NYA_PLUGIN_STEAM`, but no build rule uses them. `NYA_EXECUTION_MODE=3` is called "steam".
- To ship on Steam: a build variant linking `libsteam_api.so` / `steam_api64.dll` and adding them to the
  depots; `SteamAPI_RestartAppIfNecessary` first, init with a fallback when the client is not running,
  `SteamAPI_RunCallbacks` each frame, deinit; Steam Cloud rules for the save directory. No SteamStub DRM
  wrapper: it rewrites the exe, breaking the integrity CRC and the signature.
## `[~]` CI

Never green so far. The ccache change shipped a workflow with duplicated `if:` keys, which GitHub rejects
without starting any job; `actionlint` catches that locally. Vendors now build in one job per platform that saves the cache right after the
build, keyed on submodule revisions, vendor recipes and both toolchain directories; the check, test and
build jobs restore it.

- `[ ]` Watch the first run of that layout through on both platforms. The last Windows test run passed
  137 tests and failed on `test_command.c` running `pwd` in `/tmp`, which Windows lacks; it now runs in
  `src`.
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
