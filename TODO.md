# nyangine — what is left

`[ ]` todo · `[~]` in progress · `[⏭]` deferred

---

## Standing decisions

| Area             | Decision / Rule                                                                                                  |
| :--------------- | :--------------------------------------------------------------------------------------------------------------- |
| **Workflow**     | Edit files directly. Luca handles git. No agent runs git beyond read-only commands.                              |
| **Comments**     | _Keep the why, cut the essay._ Compress each prose block to its load-bearing claim (1–3 lines).                  |
| **Gamepad**      | Tagged digital source: `NYA_InputBinding` is a union of key \| gamepad button \| axis-past-threshold.            |
| **Subsystems**   | Unified registry: `core_system.h` handles engine subsystems and game systems. Old lifecycle/frame split is gone. |
| **Config**       | `NYA_CONFIG` is hot-reloadable from `assets/config/engine.nya`. Backed by generic reflection.                    |
| **Ceilings**     | Fixed-capacity arrays register with `nya_ceiling_register` for visibility.                                       |
| **Verification** | Every engine feature gets a caller in `gnyame`, not only a test. Verify by running the game.                     |

---

# Open

## `[ ]` Colour grading through a LUT

Not supported. The post chain (`render_post.h`) takes arbitrary pipelines and ships five effects —
bloom, blur, crt, grayscale, pixelate — and `mesh3d_tonemap` is a per-material display curve, not a
grading stage. Nothing anywhere samples a colour lookup table.

- → A `effect_lut.frag.hlsl` sampling a 3D LUT, or a 2D strip if `SDL_GPU_TEXTURETYPE_3D` turns out to
  be unevenly supported (the same reason the shadow map is a colour target rather than a sampled depth
  one — see `mesh3d_shadow.frag.hlsl`).
- → The asset system has no LUT type. A `.cube` file is the format colourists actually hand over, so
  that is the loader worth writing rather than a bespoke one.
- → It has to sit *after* tonemapping and before the bloom composite, or grading fights the shoulder.
- ⚠ Decide first whether grading belongs in a renderer whose whole premise is flat authored colour —
  see the note on `mesh3d_tonemap` about why ACES was rejected. A LUT is the same argument one step
  further on.

## `[~]` Shadows — the fit is rebuilt, the crossfade wants a second look

The cascade fit was wrong in a way that made shadows change whenever the camera did. Each cascade was a
fixed-size box a fixed distance in front of the camera, so an orbit camera further from its subject than
the near cascade's reach spent that cascade on empty air and the whole scene fell through to the
coarsest map. Cascades are now fitted to slices of the camera's own frustum, by bounding sphere so the
fit is rotation invariant. See `nya_render3d_shadow_for_camera` and `NYA_Render3DShadowFit.range`.

- `[ ]` **Verify the crossfade by eye.** The cascade boundary is now blended over the outer 15% of each
  cascade and the PCF penumbra is held constant in world units across cascades — both aimed at "the
  shadows move when the camera turns", neither confirmed on screen yet.
- `[ ]` **`nya_render3d_cascade_extent` is dead.** It was the old geometric split and nothing calls it
  now; `NYA_RENDER3D_SHADOW_CASCADE_RATIO` with it. Remove both, or keep them and say what for.
- `[ ]` **The sun moves.** `system_sky.c` turns the light every frame, so the texel grid the snap rounds
  to rotates with it and edges crawl regardless of how still the camera is. Snapping the light's own
  basis to discrete steps would fix it; nothing does yet.

## `[~]` Ceiling auditing — HUD row done, config-over-macros blocked

Registry (`core_ceiling.h`/`.c`) is done with 18 ceilings registered. The `tests/nyangine/core/test_ceiling.c`
bug is fixed: the overflow block fills relative to `nya_ceiling_count()` rather than assuming the reset
left it empty, which it never did because `nya_log_sink_add` lazily registers `"log_sinks"`.

The HUD row is in `debug_overlay.c`, fullest first, amber past 75% and red past 90%.

- ⚠ **Config over macros is blocked, not skipped.** `NYA_TWEEN_MAX`, `NYA_ENTITY_MAX` and
  `NYA_RENDER2D_FONT_CACHE_MAX` cannot become config options as things stand: `NYA_CONFIG` is a global
  **in the game DLL** (`gnyame/config.h`), so no engine module can read it. That is also why
  `shadow_bias`, `shadow_cascades` and `shadow_map_size` are declared in `NYA_ConfigEngine`, loaded from
  `engine.nya`, and read by nothing. → Either the engine owns a config instance the game embeds a
  pointer to, or these stay macros. Decide that before touching the three ceilings.

## `[~]` SDF text — works; how it looks is still unconfirmed

`text_sdf.frag.hlsl` and `NYA_RENDER2D_PIPELINE_TEXT_SDF` exist, the pipeline is chosen from what the
atlas actually holds (latched when it was built, not asked of the face at draw time), and an SDF atlas
is sampled linearly rather than nearest.

`nya_font_sdf_set` now works where it is natural to call it — at registration, before the face exists.
The request is remembered against the face it was pushed onto and applied by whichever of the font
entry points reaches a face first, or by a frame-ended hook if nothing does. Verified in the running
game: `@17` (no request) bakes `coverage`, `@28` (the title font) bakes `distance field`, which the
atlas log line now says outright. `tests/nyangine/renderer/test_render_font_sdf.c` covers it headless.

- `[ ]` **Nobody has looked at it.** That a distance field is *in use* is established; that it reads
  better than the bitmap it replaced, at the sizes and the pixel-art idiom this game uses, is not.
  `MESH3D`-style constants in the shader — `TEXT_SDF_MIN_SMOOTHING` above all — are guesses until
  someone compares the two on screen.
- ⚠ **The atlas latches the mode when it is baked and nothing rebuilds it.** Toggling a face's mode
  after text has been drawn from it leaves an atlas of the wrong kind flagged as the other. Set it at
  registration, which is what the docs say and now what works. Fixing the late case needs render2d to
  drop cached atlases for a face, which needs a headless counterpart too.
- ⚠ **The menu does not use it, and cannot.** `layers.c` draws its title through
  `nya_render2d_text_with_font(GNY_MENU_FONT, 44, ...)` — the immediate-mode path, which takes a path
  and a size and never touches the NYA_Font registry the request table is keyed against. Only the 2D
  scene's HUD uses the registered `"title"` font. Either the menu moves onto NYA_Font, or the request
  table has to sit lower down.

## `[ ]` A general cache: keyed memory blobs, one implementation

Every subsystem that needed a cache has grown its own, and each one is a fixed array with a hand
written lookup, its own eviction rule (usually none), and its own idea of when an entry is stale:

| Where | What it holds | Keyed by |
| :--- | :--- | :--- |
| `core_asset.c:407` `_nya_asset_lookup` | the last asset per slot | handle pointer, plus a copy of its text |
| `render_text.c:262` `_nya_text_font_handle_intern` | stable handle strings | linear scan over the text |
| `render2d.c:319` `_nya_render2d_font_cache` | glyph atlases | path + point size, linear scan |
| `render3d.h` mesh registry | uploaded vertex buffers | a name |

Four hand-rolled tables, and between them they have produced most of the bugs in the Findings below —
a memo that returned the wrong asset for two days, an intern table that exists only to paper over that
memo, an atlas cache that latches state at bake time and cannot be told the state changed. There is
also at least one cache that *should* exist and does not: shaped text runs, re-derived every frame for
strings that are literals (see the budgets section).

So: one `base_cache.h` — memory blobs in, key out, and the awkward parts solved once.

### What it has to get right, because the existing ones did not

- **Keys are content, never pointers.** `nya_asset_get` memoized on the handle pointer on the
  reasoning that the same pointer is the same thing. It is not, for a key built into a caller's
  buffer, and the failure is silent and intermittent. A cache that takes `(const void*, u64)` and
  hashes it cannot be used that way by accident.
- **Invalidation is a first-class input, not a convention.** The asset memo has a generation counter
  bumped whenever the dictionary rehashes; the atlas cache compares a `TTF_Font*` to notice a reload;
  `nya_font_sdf`'s request table remembers the face it last wrote to. Three mechanisms for one idea.
  A cache entry should be able to carry a generation the owner bumps, and the owner should not have to
  invent the bookkeeping.
- **Eviction has to exist.** None of the four evict. They fill and then either warn and refuse
  (the asset intern table) or silently reuse the last slot. A cache whose only policy is "full" is a
  fixed array with extra steps, and choosing between LRU and refuse-when-full should be a field.
- **It is a ceiling like everything else.** `nya_ceiling_register` so the HUD shows how full each
  cache is, which is the row that would have shown the glyph atlas cache thrashing.

### The questions to settle before writing it

- **`base/` or `core/`?** `base/` means SDL-free and reachable from the build tool and from a headless
  test, which is where the asset memo's bug would have been caught. It also means the cache cannot
  own anything GPU-side, so the atlas cache stays where it is and only borrows the lookup. That split
  is probably right but it does mean the biggest existing caches are not the first callers.
- **Who owns the blob?** An arena-backed cache that copies is simplest and matches how the engine
  allocates, but a glyph atlas is a GPU texture and a shaped run is thirty kilobytes a caller already
  keeps. A cache of *handles* with a caller-supplied destructor is more useful and much easier to get
  wrong.
- **Thread safety.** `core_job.h` runs work on other threads and asset loading is queued from the
  caller's thread. A cache with no story here will be used from a job eventually and the bug will not
  reproduce.
- **What it costs.** The asset memo exists because `nya_asset_get` plus its hash were 1.28% of a
  profile, and a general cache that hashes where the memo compared a pointer would give that back —
  see the siphash finding. Whatever this becomes has to be measured against `bench/bench_core.c`'s
  existing asset-lookup case before anything moves onto it.

## `[ ]` Budgets — measured, and where the waste is

Measured on the 3D scene, release build, 1280x720, 4x MSAA: **116 MB RSS**, roughly **300 MB VRAM**,
and a release `perf` profile whose top entries are below. Everything here is a choice rather than a
leak, and the ranking is by what a fix returns for what it costs.

### The glyph atlas is paid for three times, in the wrong format

Each atlas allocates its full size as an RGBA8 GPU texture, *again* as an `SDL_Surface` kept alive for
later bakes, and *again* as a transfer buffer sized for the whole atlas. Four live atlases are 19.4 MB
of pixels, so about **58 MB across RAM and VRAM**.

The pixels are single-channel: the blit fills every channel with the same coverage. R8_UNORM cuts all
three by four — **58 MB → 14.5 MB**, the single largest saving available.

- ⚠ Not a one-line change. `text_sdf.frag.hlsl` already reads `.r`, but the *coverage* path goes
  through NYA_RENDER2D_PIPELINE_TEXTURED, which samples RGBA and multiplies — an R8 atlas there draws
  red text. It needs a coverage-from-`.r` text pipeline shaped like the SDF one.
- `[ ]` **The transfer buffer is sized for the whole atlas** and one glyph is baked at a time. A
  cell-sized staging buffer with a rect upload is another 19 MB (5 MB after R8).
- `[ ]` **The grid is 512 cells sized to the largest glyph.** `@44` is 1152x2176 for 512 slots, and a
  game uses a fraction of them. A smaller NYA_RENDER2D_GLYPH_CAPACITY costs nothing but a ceiling.

### The scene is emitted four times a frame

Three shadow cascades plus the camera pass, and the batch keeps no geometry to replay, so every draw
call is regenerated from scratch each time. That multiplier is behind most of the profile:

| Symbol | Share | Note |
| :--- | ---: | :--- |
| `b3SolveContacts_Mesh` | 4.3% | Box3D against the terrain as a *triangle mesh* |
| `nya_render3d_sphere` | 3.5% | tessellated per draw, per pass |
| `VULKAN_UploadToBuffer` | 2.4% | four uploads of the same geometry |
| `_nya_render2d_quad` | 1.6% | |
| `nya_render2d_text_with_font` | 1.2% | shaping re-run per string per frame |
| `nya_render3d_quad` | 1.2% | |
| `nya_render3d_sort_keys` | 1.0% | the shadow pass sorts transparencies it does not need |

- `[ ]` **`nya_render3d_sphere` is the clearest win.** 24 segments by 12 rings is 1152 vertices, built
  on the CPU, for each of four lamp markers, in each of four passes — about 18k vertices a frame,
  identical every time. `nya_render3d_mesh_register` already exists: register a unit sphere once and
  draw it transformed. The call site keeps the same signature, so the ergonomics do not change.
- `[ ]` **The shadow pass does not need the sort, the UVs, the colours or the normals.** It writes
  depth. A depth-only vertex format would cut the upload it dominates.
- `[ ]` **Terrain physics is a triangle mesh.** A heightfield is the shape Box3D has a cheaper solver
  for, and the terrain is literally a heightfield.
- `[ ]` **Cascade count is a direct multiplier** on all of the above. Three may be one more than this
  scene needs now that the cascades are fitted to the frustum.

### ✅ `NYA_Vertex3D` is 36 bytes, was 64

Done. `f32x3` is an `ext_vector_type(3)` and therefore **sixteen** bytes rather than twelve, so two of
them wasted eight bytes before the four-float colour was counted.

| Field | Was | Now | Format |
| :--- | ---: | ---: | :--- |
| position | 16 | 12 | FLOAT3, three plain floats |
| uv | 8 | 4 | HALF2 |
| normals | 16 | 12 | FLOAT3 |
| color | 16 | 8 | HALF4 |
| | **64** | **36** | |

**No shader changed.** HALF2 and HALF4 are expanded by the input assembler, so the shaders still read
the `float2`/`float4` they always did. Callers build one with `nya_vertex3d(position, color, normal, uv)`
and read the position back with `nya_vertex3d_position`, which is the only thing that knows the storage
is packed — nine construction sites and five read sites, all narrower to write than the designated
initializers they replaced.

Two things the plan got wrong, both caught before they shipped:

- ⚠ **Colour cannot be UBYTE4_NORM**, which is what the plan said and what NYA_Vertex2D does. A vertex
  colour above one is how an emissive surface is pushed past the bloom threshold, and
  `GNY_CUBE3D_FIRE_COLOR_START` is 1.15 red for exactly that reason. Normalized bytes clamp it and the
  flame stops glowing — a look change no test would have caught. HALF4 keeps everything to 65504.
- ⚠ **The helpers must live outside `#if NYA_HEADLESS_ENABLED`.** renderer.c splits on it and the first
  attempt put them in the headless branch, which links in one build and not the other.

`[ ]` **Normals could still be four bytes** — octahedral SNORM16x2 takes this to 28. Not taken because
`mesh3d_edge` finds edges with `fwidth` of the interpolated normal, and a derivative of a quantised
value is a different thing. Worth measuring, not worth assuming.

`[ ]` **`NYA_VertexSkinned3D` is untouched** and still carries the same four wide fields plus bones and
weights. It has its own attribute table and its own layout, so it can move independently.

### The asset blob is 12 MB, uncompressed, and LZ4 is already linked

`NYA_ASSET_BLOB` is the largest object in the binary by a factor of ten — everything else static
together is about 4 MB. `src/build/pp/asset.c` writes it as plain bytes, and `vendor/lz4` is already
built and on the link line for every target.

- `[ ]` Compress per entry and decompress on first use. The blob is `.rodata`, so what is resident is
  whatever has been touched — but the binary is 12 MB larger on disk and in the page cache regardless.
- `[ ]` Or bake less: nothing checks whether an asset is ever loaded, so the blob carries the whole
  tree including anything only a test or an old scene reads.

### Static memory that is resident for no reason

From `nm --size-sort` on the release binary:

| Object | Size | |
| :--- | ---: | :--- |
| `NYA_ASSET_BLOB` | 12 MB | above |
| `b3_worlds` / `b2_worlds` | 596 + 344 KB | solver pools, both resident whether or not a scene uses them |
| `nya_system_entity_lights.candidates` | 384 KB | `static NYA_EntityLightEntry[NYA_ENTITY_MAX]` |
| `_NYA_NET_CLIENT` / `_NYA_NET_SERVER` | 260 + 79 KB | resident in single player |
| `_nya_audio_system` | 436 KB | in `.data`, not `.bss` |
| `nya_system_entity_render_in.entries` | 192 KB | `static NYA_EntityDrawEntry[NYA_ENTITY_MAX]` |
| `_nya_render2d_font_cache` | 147 KB | atlas metadata |

- `[ ]` **The two entity scratch arrays are per-frame working memory held forever** — 576 KB of
  function statics that exist because there was nowhere else to put them. The frame arena is exactly
  that place, and it would also stop them being sized by `NYA_ENTITY_MAX` rather than by what a frame
  actually draws.
- `[ ]` **`_nya_audio_system` is in `.data` because it has non-zero initializers**, so it costs its
  436 KB in the binary *and* a copy at load. Whatever the non-zero defaults are, setting them in
  `nya_system_audio_init` moves the whole thing to `.bss`.

### VRAM, by allocation

| What | Size | |
| :--- | ---: | :--- |
| Swapchain MSAA colour + depth | 28 MB | 4x, D24S8 |
| Shadow atlas colour + depth | 32 MB | 2048², R32_FLOAT |
| Glyph atlases | 19 MB | see above |
| Each offscreen render texture | up to 32 MB | colour **plus its own** MSAA colour and MSAA depth |
| Refraction capture | 3.5 MB | full-resolution copy |
| Batch + transfer buffers | 3.8 MB | |

- `[ ]` **The shadow atlas wastes a quadrant** — three cascades in a 2x2 leaves a quarter of colour and
  depth unused: **8 MB**. A 3-wide strip spends it.
- `[ ]` **R32_FLOAT for a depth in [0, 1]** — R16 halves it to 8 MB. Measure acne on the near cascade
  first; that is where sixteen bits would show if anywhere.
- `[ ]` **Every render texture carries its own MSAA colour and depth**, 29 MB on top of the 3.5 the
  target costs. A target that exists only to be post-processed may not need multisampling.

### Text is re-shaped every frame, including text that never changes

From a release profile of the **main menu**, which draws six constant strings: `TTF_UpdateText` 2.3%,
`GetWrappedLines` 1.3%, `TTF_Size_Internal` 1.1% — about **5% of the profile re-deriving a layout that
cannot have changed**, plus the `malloc`/`realloc`/`cfree` traffic underneath it, which is also visible.

`bench/bench_text.c` prices one HUD frame at 43 µs, which is cheap enough that shaping on demand is the
right default. What is not right is doing it sixty times a second for a string that is a literal. A
shaped-run cache keyed on (face, size, text, wrap) would take it to nothing, and the invalidation is
the same one the atlas already does: the TTF_Font pointer changing.

### Measurement

- ✅ **Profiling now uses the release build.** `./build run profile` records it; `./build perf` reads
  it. It used to be `./build run debug` — the `-O0` build with four sanitizers — which measured the
  instrumentation rather than the game. See run_profile in `src/build/misc.h`.
- `[ ]` **GPU allocations should register with the ceiling registry.** It exists for exactly this, the
  HUD already lists arenas beside it, and a hand-computed VRAM figure is one that goes stale.
- `[ ]` **RSS has no breakdown.** 116 MB is measured; which arenas hold it is not. The debug overlay
  totals them per frame and nothing records that anywhere a profile can reach.

## `[ ]` UI system

`src/nyangine/ui/ui.c` is one line, `ui.h` is `#pragma once`, and the include is commented out in
`nyangine.c`. `docs/index.org` has wanted "widgets, layout, styling" and a debug UI since the start.
The game builds its menus by hand in `layers.c` in the meantime.

## `[ ]` Editor

`src/nyangine/editor/editor.c` and `.h` are both empty files. A directory and a name, no plan.

## `[ ]` Steam is dead code, twice over

- `net_steam.c:57` returns `NYA_ERROR_NOT_SUPPORTED`; the transport is unimplemented.
- `plugins/steam/steam.c` has never been compiled. `plugins.c` *does* include it, behind
  `#ifdef NYA_PLUGIN_STEAM` — but `FLAGS_PLUGINS` in `src/build/flags.h:61` passes CURL, SQLITE,
  DISCORD and LUA and not STEAM, so no build defines it. (steam.c's own file comment claims it is in no
  translation unit at all; that is stale, and wrong in a way that would send the next person looking in
  `nyangine.c` instead of at the build flags.) `NYA_EXECUTION_MODE=3` is called "steam".

## `[ ]` The game side has no tests

`tests/gnyame/` holds a `.keep` and nothing else.

## `[ ]` The verification rule is not being kept

*Every engine feature gets a caller in `gnyame`* — but nothing in the game touches nn/DQN/NEAT, skeletons,
saves, nav, jobs, occlusion, tweens, LOD, gamepads, or the sqlite/curl/discord/steam plugins. The nn one
matters most: the GDD makes DQN and NEAT robot programming the core mechanic and the game uses neither.

## `[ ]` Hot reload can open a half-written DLL

`src/main.c:108`. Wants the asset system's change detection to gate the load rather than racing the
compiler.

## `[ ]` The glyph atlas rasteriser is still untested

The findings below record why — it needs a device — and that the same unsigned-overflow bug landed twice
because of it. Only the postmortem is written down; nothing has made the bake, upload and lookup
reachable from a test.

# nyangine — Research & Findings

This file contains non-obvious engineering findings and details that cost real effort to learn, captured to prevent re-derivation.

---

## Findings

### "Has A Shadow Pass Run" Is Not "Am I In A Shadow Pass"

`nya_render3d_shadow_active` answers the first. It goes true at `nya_render3d_shadow_end` and stays
true until `nya_render3d_end`, so across one frame it is **false during the frame's first shadow pass
and true for the whole camera pass** — the exact inverse of what a caller wanting to skip the shadow
pass needs.

`nya_particles_draw` used it to mean the second. The result was that every particle system which had
not opted into casting shadows was drawn into cascade zero's shadow map and skipped in the pass that
draws to the screen: particles cast shadows and were themselves invisible. The 3D scene lost its fire
plume, its smoke and its dust, and the comment directly above the test described the behaviour it was
supposed to have rather than the one it had.

Nothing caught it because both readings compile, both names read plausibly, and the failure is silent
in the only direction anyone looks — the thing is missing, not wrong. `nya_render3d_shadow_pass_active`
now exists for the second question, and both are documented against each other.

### A Pointer-Keyed Memo Cannot Memoize A Handle Built On The Stack

`nya_asset_get` memoized on the handle *pointer*, reasoning that "the same pointer with the dictionary
unchanged is necessarily the same asset". That holds for a string literal and fails for a handle built
into a caller-owned buffer: the stack slot is reused, so two different handles hold the same address
one after the other and the memo matches on address alone.

It went wrong in two directions at once, and neither announced itself:

- **Wrong answers.** render2d builds `"path@size"` into a local for every font lookup, so asking for a
  face at one size returned the face at whichever size was asked for last. A menu's items measured at
  the title's point size; later, a glyph atlas took the *other* font's distance-field flag and baked
  itself as the wrong kind. render_text.c had already hit this and interned its handles to work
  around it, with a note saying the real fix belonged in the asset system.
- **Then, once the memo compared content, a slow path.** Correctness alone is not enough: two sizes
  drawn every frame still shared one stack address and therefore one memo slot, evicting each other
  every time. `nya_siphash` went to the top of the profile at 5%, above every part of drawing.

Both halves need both fixes. The memo keeps a *copy* of the handle text and compares that — comparing
the stored pointer against the incoming one is comparing a buffer with itself and always agreeing —
and the caller interns its handles so each (path, size) pair has an address of its own, which is a
memo slot of its own. 5.06% to 3.73%, and what remains is the startup integrity MAC rather than
anything per frame.

### The Profile Was Of The Sanitizers

`./build run debug` was the only thing that ever ran under perf, and that is the `-O0` build with
four sanitizers — so `./build perf` showed where the instrumentation spent its time, not the game.
`misc.h` said as much in a comment and the command was left pointing there anyway, which is the worst
of both: a profile that exists, is wrong, and says so somewhere nobody reads.

Two things fell out of profiling the release build instead. The top entry of the 3D scene is
`nya_render3d_sphere` at 3.5% — four lamp markers tessellated into 1152 vertices each, in each of four
passes, identical every frame. And the top entry of the menu is text shaping, on strings that are
literals. Neither is visible at all in the debug profile, where the sanitizer bookkeeping dominates
everything.

### Cascaded Shadows Are Fitted To The Frustum, Not To A Constant

A cascade sized from a constant and placed a fixed distance in front of the camera only works when the
camera is close to what it is looking at. `GNY_CUBE3D_SHADOW_EXTENT` was 0.16 of the terrain's extent
and the orbit camera sits well outside that, so cascades zero and one covered nothing but air and the
whole scene was shadowed by the coarsest map — and which patch of ground fell into which cascade moved
with the camera, which is what "the shadows change when I move" was.

The fix is the standard one and the part worth writing down is *why each piece is not optional*:

- Cascades take **slices of the camera's own frustum**, so their size follows the view rather than a
  constant, at any distance from anything.
- Each is sized by the slice's **bounding sphere, not its bounding box**. A box fitted to the frustum
  corners changes size as the camera turns, so the map's texels change size every frame and the texel
  snap has no fixed grid to round to — the edges then crawl however carefully they are rounded.
- The shader picks a cascade by **projecting into each volume in turn**, not by distance from the
  camera. A cascade is not a sphere around the viewer and a distance test does not describe where one
  is.

Three debug views found it, in this order, and only the last was conclusive: shadows off (the dark
region was entirely spurious), the visibility term as greyscale (hard-edged, so a volume boundary and
not a caster), and the cascade index as colour (cascade zero appeared nowhere on the ground).

### What Shaping Costs

`TTF_CreateText` per string per draw, which replaced a memoised per-codepoint lookup, is affordable:
**43.4 µs for a twenty-line HUD**, about 0.26% of a 16.7 ms frame, at ~71 ns per glyph. One short line
is 950 ns and measuring one without keeping the glyphs is 700 ns. Wrapping is the expensive part — the
same paragraph costs 6x more wrapped than unwrapped, because the shaper does the line breaking. See
`bench/bench_text.c`; the answer is that a memo would buy nothing worth the staleness.

### Codegen Dependencies Were Missing From Half The Build

Generated sources — `src/generated/strings.h`, `reflection.c`, `assets.h` — are produced by metarules
that a compile rule has to *depend on*, and the dependency was on the DLL rules and not on the
executable ones, nor on the test or benchmark rules. Everything still built, because a normal build
regenerates them for the DLL first and the executable happens to compile afterwards. It surfaces the
moment the generated file is genuinely new: adding a shader made the executable fail to compile against
an `assets.h` that the DLL beside it had already been given.

`nya_build_parallel` opens one epoch per call and builds a shared dependency once inside it, so naming
the codegen on every rule that reads it costs nothing — which is why the fix is a line per rule rather
than a prebuild step.

### Clang Vector Miscompile (`f128x3`)

`f128x3` is a clang miscompile, not a matrix bug. `ext_vector_type(3)` over x87 `long double`: clang sizes the stack slot from the packed `<3 x x86_fp80>` (32 bytes) and emits every store as the padded four-lane form (40 bytes). Two and four lanes are fine, and no other element type has a non-power-of-two store size. Fixed by declaring `f128x3` with a fourth lane — `sizeof` was already 64. Reproduced standalone on clang 22.1.8.

### Box2D Pre-solve Traps

- `enablePreSolveEvents` is per shape and applies to the **dynamic** body, not to the platform — Box2D ignores it on a static shape.
- Box2D **recycles contacts that have not moved**, skipping `b2UpdateContact` and therefore the callback entirely. A body resting on a platform is exactly that case, so a drop-through request is stored and never read. Recycling is suspended while any window is open, via `b2World_SetContactRecycleDistance`.

### `TTF_CreateText` Layout with Null Engine

`TTF_CreateText` accepts a null `TTF_TextEngine` and still runs the full layout. An engine is only what _draws_ a laid-out text. That is what lets shaping — and therefore text measurement — work with no GPU, which `render_text.c` and the headless text path are built on. Shaping outputs glyph _indices_, so an atlas consuming it must be keyed by index; there is no public codepoint→index mapping, which is why the eager ASCII block had to go.

### LuaJIT Allocator on x64

LuaJIT cannot take an arena allocator on x64. Its collector needs the heap in the low 2 GB and ships an mmap allocator to guarantee it; a custom allocator is documented upstream as unsupported and fails at runtime. The one place in the engine that is not arena-backed, and not a choice.

### Physics Solver Order

The physics solver steps _before_ the entity update. So a velocity set during entity update is not consumed until the next tick — which is why zeroing a kinematic move's velocity on the arrival tick silently dropped its final step.

### Terrain LOD

Three things that each look right alone but compounded to strip detail off everything including what was underfoot:

- Distance must be measured to a chunk's **bounding sphere**, not its centre, or a chunk the camera is inside reads as half a chunk away.
- The skirt depth bound is the terrain's **relief**, not a multiple of the cell — a cell multiple inflates every bounding radius.
- The band scale is the **chunk width**, not a fraction of the world's extent.

### Testing Glyph Atlas

The test suite structurally cannot reach the glyph atlas because it needs a device, so the bake, upload, and lookup are untested. This led to the same unsigned integer overflow bug twice: a hash whose multiply wraps in a build compiled with `-fno-sanitize-recover=all` needs `__attr_no_sanitize("unsigned-integer-overflow")`. Once in the kerning memo, once in the glyph lookup. Both aborted the first frame that drew a character; both were found by running the game. The shaping half is now CPU-only and covered; the rasterising half is not.
