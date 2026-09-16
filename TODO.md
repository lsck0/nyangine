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

18 ceilings are registered and shown in `debug_overlay.c`, fullest first, amber past 75%, red past 90%.

- Blocked: `NYA_TWEEN_MAX`, `NYA_ENTITY_MAX` and `NYA_RENDER2D_FONT_CACHE_MAX` cannot become config,
  because `NYA_CONFIG` is a global in the game DLL (`gnyame/config.h`) that no engine module can read.
  The same is why `shadow_bias`, `shadow_cascades` and `shadow_map_size` are loaded and read by nothing.
  Either the engine owns the config instance or these stay macros.
- `[ ]` `NYA_CONFIG` resets to zero after a code hot reload: `gnyame_init` does not run again, and the
  config watch keeps a pointer into the unloaded DLL. Same root cause as above.
- `[ ]` `NYA_CONFIG.game.player_speed` is not read; `gny_net_apply_command` uses `GNY_PLAYER_SPEED`.

## `[~]` SDF text: works, looks unconfirmed

`text_sdf.frag.hlsl` and `NYA_RENDER2D_PIPELINE_TEXT_SDF` exist; the pipeline follows what the atlas was
baked as, and SDF atlases sample linearly. `nya_font_sdf_set` works at registration. Verified in game:
`@17` bakes coverage, `@28` (title) bakes a distance field. `test_render_font_sdf.c` covers it headless.

- `[ ]` Nobody has compared it on screen with the bitmap it replaced. `TEXT_SDF_MIN_SMOOTHING` and the
  other shader constants are guesses.
- The atlas latches its mode at bake time. Changing the mode after drawing leaves a wrong atlas; fixing
  that needs render2d, and its headless counterpart, to drop cached atlases for a face.
- The menu cannot use it: `layers.c` draws through `nya_render2d_text_with_font(GNY_MENU_FONT, 44, ...)`,
  which bypasses the NYA_Font registry the requests are keyed on. Move the menu to NYA_Font, or key
  requests lower down.

## `[ ]` One general cache

Each subsystem grew its own fixed array cache with its own lookup, no eviction and its own staleness
rule:

| Where                                   | Holds                   | Keyed by                         |
| :-------------------------------------- | :---------------------- | :------------------------------- |
| `core_asset.c` `_nya_asset_lookup`      | last asset per slot     | handle pointer slot, text copy   |
| `render2d.c` `_nya_render2d_font_cache` | glyph atlases           | path and point size, linear scan |
| `render3d.h` mesh registry              | uploaded vertex buffers | handle copy, FNV-1a then text    |

They produced several findings below. Shaped text runs should be cached and are not (see budgets).

A `base_cache.h` has to:

- Key on content, never pointers: take `(const void*, u64)` and hash.
- Take invalidation as input. Today there is a generation counter, a `TTF_Font*` comparison and a
  remembered face, three mechanisms for one idea.
- Evict. None of the current ones do; LRU or refuse-when-full should be a field.
- Register as a ceiling.

Open questions:

- `base/` (SDL free, testable headless, cannot own GPU objects) or `core/`.
- Copying into an arena, or holding handles with a caller destructor.
- Thread safety, since jobs will use it.
- Cost: the asset memo exists because lookup plus hash was 1.28% of a profile. Measure against
  `bench/bench_core.c` before moving anything onto it.

## `[ ]` Budgets

3D scene, release, 1280x720, 4x MSAA: 116 MB RSS, about 300 MB VRAM.

### Glyph atlas

Atlases are R8 coverage (58 MB down to 14.5 MB across RAM and VRAM), with `NYA_RENDER2D_PIPELINE_TEXT`
for coverage text.

- `[ ]` The transfer buffer is sized for the whole atlas while one glyph bakes at a time. A cell sized
  buffer with a rect upload saves 4.8 MB.
- `[ ]` 512 cells sized to the largest glyph: `@44` is 1152x2176. A smaller NYA_RENDER2D_GLYPH_CAPACITY
  costs only a ceiling.

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
half float because emissive colours exceed one (`GNY_CUBE3D_FIRE_COLOR_START` is 1.15 red). The helpers
sit outside `#if NYA_HEADLESS_ENABLED` so both builds link.

- `[ ]` Octahedral SNORM16x2 normals would reach 28 bytes, but `mesh3d_edge` takes `fwidth` of the
  interpolated normal. Measure first.
- `[ ]` `NYA_VertexSkinned3D` still has the wide layout.

### Asset blob

Entries are LZ4 compressed when that saves `NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES`, expanded once
and shared by reference count.

- `[ ]` Bake less: nothing checks whether an asset is ever loaded.

### Static memory

From `nm --size-sort -S` on the release binary:

| Object                                | Size         |                               |
| :------------------------------------ | -----------: | :---------------------------- |
| `NYA_ASSET_BLOB`                      | 12.4 MB      | `.rodata`                     |
| `b3_worlds` / `b2_worlds`             | 596 + 344 KB | resident even if unused       |
| `_nya_audio_system`                   | 446 KB       | `.bss`                        |
| `_NYA_NET_CLIENT` / `_NYA_NET_SERVER` | 266 + 80 KB  | resident in single player     |
| `dphaseTable` / `tllTable`            | 256 + 128 KB | vendored audio decoder tables |
| `_nya_render2d_font_cache`            | 150 KB       | atlas metadata                |

- `[ ]` The solver pools are the largest statics and do not shrink when a scene uses one dimension.

### VRAM

| What                          | Size         |                                   |
| :---------------------------- | -----------: | :-------------------------------- |
| Swapchain MSAA colour + depth | 28 MB        | 4x, D24S8                         |
| Shadow atlas colour + depth   | 19 MB        | strip, R16_UNORM + D24S8          |
| Glyph atlases                 | 4.8 MB       | R8                                |
| Each offscreen render texture | up to 32 MB  | colour plus its own MSAA and depth |
| Refraction capture            | 3.5 MB       | full resolution copy              |
| Batch + transfer buffers      | 3.8 MB       |                                   |

- `[~]` Render textures can skip depth (`NYA_RENDER_TEXTURE_DEPTH_NONE`, used by the post chain). The
  MSAA half needs single sampled pipeline variants, which
  `NYA_AssetLoadParameters.as_graphics_pipeline.single_sampled` supports.

### Text is reshaped every frame

The main menu draws six constant strings and spends about 5% of its profile in `TTF_UpdateText`,
`GetWrappedLines` and `TTF_Size_Internal`, plus the allocations beneath. `bench/bench_text.c` prices a
HUD frame at 43 µs, so shaping on demand is fine, but a run cache keyed on (face, size, text, wrap) and
invalidated by the `TTF_Font*` would remove it.

### Measurement

- `[ ]` GPU allocations should register with the ceiling registry instead of hand computed VRAM figures.
- `[ ]` RSS has no breakdown by arena anywhere a profile can reach.

## `[ ]` UI system

`src/nyangine/ui/ui.c` is one line, `ui.h` is `#pragma once`, and the include is commented out in
`nyangine.c`. The game builds menus by hand in `layers.c`.

## `[ ]` Editor

`src/nyangine/editor/editor.c` and `.h` are empty.

## `[ ]` Steam is dead code

- `net_steam.c` returns `NYA_ERROR_NOT_SUPPORTED`.
- `plugins/steam/steam.c` has never been compiled: `FLAGS_STEAM_*` in `src/build/flags.h` define
  `NYA_PLUGIN_STEAM`, but no build rule uses them. `NYA_EXECUTION_MODE=3` is called "steam".

## `[ ]` The game side has no tests

`tests/gnyame/` holds a `.keep`.

## `[~]` CI

Never green so far. Vendors now build in one job per platform that saves the cache right after the
build, keyed on submodule revisions, vendor recipes and both toolchain directories; the check, test and
build jobs restore it.

- `[ ]` Watch the first run of that layout through on both platforms. Windows has only recently got
  past sqlite, SDL_net, box2d and LuaJIT, so more native build issues may follow.
- `[ ]` Engine, game and tests are not cached. Each rule is one `clang` call that compiles and links,
  which ccache cannot help, and each test is its own unity build. Split compile and link, or build the
  engine once as an object the tests link, then cache with ccache.
- `[ ]` `nya_build_parallel` waits for a whole batch of `max_jobs`, so one slow rule idles the rest. Use
  a pool that refills as rules finish.
- `[ ]` No clang-format gate: the tree does not satisfy `.clang-format`, and fixing that is a 73k line
  reformat with include regrouping to review.

## `[ ]` The verification rule is not being kept

Nothing in the game touches nn/DQN/NEAT, skeletons, saves, nav, jobs, occlusion, LOD, gamepads, or the
sqlite, curl, discord and steam plugins. nn matters most: the GDD makes DQN and NEAT robot programming
the core mechanic.

## `[ ]` Animation has no caller in the game

Skeletons, skinned meshes and the sprite animator are only reached from tests. `bender.fbx` is loaded
by `test_skeleton.c` and never drawn. Bone order, multi-mesh skins, event direction and root motion are
covered headless; a skinned draw on screen is not.

## `[ ]` The glyph atlas rasteriser is untested

It needs a device, and the same unsigned overflow bug shipped twice because of it. The R8 bake is the
newest untested code in the tree, verified only by reading the screen.

## `[ ]` No test reaches `render3d.c`

Tests build headless, which swaps in `render3d_headless.c`, so `_nya_render3d_visible`, the frustum
build and culling are uncovered. Move culling into a unit both builds include, or add a non-headless
test target.

---

# Findings

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
