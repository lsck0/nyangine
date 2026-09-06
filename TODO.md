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

## `[~]` Ceiling auditing — registry done, HUD row and config-over-macros open

Registry (`core_ceiling.h`/`.c`) is done with 18 ceilings registered. Actual game runs clean.

- ⚠ **`tests/nyangine/core/test_ceiling.c` bug:** test fails because lazy registration of other modules happens during test, violating the assumption of an empty registry. Needs a reset-for-test hook or relative fill target.
- `[ ]` **HUD row:** Add debug HUD row in `debug_overlay.c`, sorted by fullness.
- `[ ]` **Config over macros:** Transition tween pool, entity count, and font cache to config option.

## `[ ]` Prebuild step — `./build run test` bypasses codegen

- Editing `assets/i18n/en.json` or `@reflect` before running tests builds against stale files because test rules bypass codegen dependencies (found in `on_linux/build_linux.h`).
- → Give the test rules the same codegen dependencies as project rules, or add an explicit `prebuild` rule.

## `[ ]` SDF fragment shader

- Font atlas stores distance field, but text shader samples it as a picture.
- → Needs a fragment shader thresholding the field (reference: `vendor/sdl-ttf/examples/testgputext/shaders/shader-sdf.frag.hlsl` in register convention).

## `[ ]` Text benchmark

- `TTF_CreateText` per string per draw replaced a memoised lookup; needs a benchmark pricing it in `./build run bench` (with a HUD's worth of strings per frame).

# nyangine — Research & Findings

This file contains non-obvious engineering findings and details that cost real effort to learn, captured to prevent re-derivation.

---

## Findings

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
