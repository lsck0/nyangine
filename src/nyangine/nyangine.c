#include "nyangine/base/base.c"
#include "nyangine/math/math.c"
#include "nyangine/nn/nn.c"
#include "nyangine/platform/platform.c"
#include "nyangine/serde/serde.c"
// Each plugin is behind its own NYA_PLUGIN_* flag; see plugins.h.
#include "nyangine/plugins/plugins.c"

#ifndef NYA_NO_SDL
#include "nyangine/physics/physics.c"
#include "nyangine/net/net.c"
#include "nyangine/core/core.c"
// before either renderer and in both: no GPU state, and headless tests reach it. render_camera.c holds
// the camera arithmetic both renderers share.
#include "nyangine/renderer/render_camera.c"
// before everything that asks it what is switched on, and in both builds for the same reason.
#include "nyangine/renderer/render_features.c"
// both builds: asset loading creates textures through it, and headless tests read its counts.
#include "nyangine/renderer/render_gpu_memory.c"
// Shaping, and in both builds for the same reason: it is CPU only, so a headless build can
// and does lay text out exactly as the real one draws it. See render_text.h.
#include "nyangine/renderer/render_text.c"
// the CPU side of glyph atlases: cells and the coverage bake. Both builds, so a headless test reaches the bake.
#include "nyangine/renderer/render_glyph_atlas.c"
// where a shadow cascade goes, not how it is rasterised. Pure math, so both builds get it.
#include "nyangine/renderer/render_shadow.c"
// parsing a lookup table is CPU only, and the asset loader in both builds reaches it.
#include "nyangine/renderer/render_lut.c"
#include "nyangine/renderer/render_sort.c"
#include "nyangine/renderer/render_lod.c"
#include "nyangine/renderer/render_occlusion.c"
// after the occlusion buffer it tests against. CPU only, so headless tests reach the culling render3d.c runs.
#include "nyangine/renderer/render_cull.c"
// both builds: where a nine-slice's pieces go is arithmetic a headless test checks.
#include "nyangine/renderer/render_nine_slice.c"
// One 2D backend, picked at build time, so nothing carries another one's code. The terminal arm is
// checked first because NYA_TERMINAL implies NYA_HEADLESS; see base_basic.h.
#if NYA_TERMINAL_ENABLED
#include "nyangine/renderer/render2d_terminal.c"
#elif NYA_HEADLESS_ENABLED
#include "nyangine/renderer/render2d_headless.c"
#else
#include "nyangine/renderer/render2d.c"
#endif
// both builds: a veil of haze is a gradient rect, drawn through whichever renderer was picked.
#include "nyangine/renderer/render2d_haze.c"
// After both renderers: particles draw through whichever the system is set to.
#include "nyangine/renderer/render_particles.c"
// After both renderers, for the reason particles are: a 2D volume draws through render2d and a 3D one
// through render3d, and the solver itself is CPU only, so a headless test steps and draws it.
#include "nyangine/renderer/render_fluid.c"
#include "nyangine/nn/nn_draw.c"
#include "nyangine/nn/nn_neat_draw.c"
#include "nyangine/debug/debug.c"
#include "nyangine/renderer/render2d_sprite.c"
// After render2d.c: the real 3D flush reuses its pass suspend and resume, which are internal to that
// translation unit. Picked to match, so a headless build has no render pass on either side.
#if NYA_HEADLESS_ENABLED
#include "nyangine/renderer/render3d_headless.c"
#else
#include "nyangine/renderer/render3d.c"
#endif
// After render3d.c, whose flush draws them and whose internals they bind through.
#include "nyangine/renderer/render3d_decal.c"
#include "nyangine/renderer/render_output.c"
// After both 2D renderers: the post chain draws through whichever one the build selected.
#include "nyangine/renderer/render_post.c"
// After whichever 2D renderer was selected: it forwards to that renderer's text API.
#include "nyangine/renderer/render_font.c"
#include "nyangine/renderer/renderer.c"
#if !NYA_HEADLESS_ENABLED
#include "nyangine/renderer/render_trace.c"
#endif
// ui.c first: it defines the module's one static state, which every other ui_*.c file reads. The rest are
// independent of each other and only ordered to read alphabetically.
#include "nyangine/ui/ui.c"
#include "nyangine/ui/ui_draw.c"
#include "nyangine/ui/ui_input.c"
#include "nyangine/ui/ui_layout.c"
#include "nyangine/ui/ui_style.c"
#include "nyangine/ui/ui_text.c"
#include "nyangine/ui/ui_widgets.c"

// For the reason testing.h is included last.
#include "nyangine/testing/testing.c"

// Last: every type it describes has to be complete, and it names sizeof and offsetof on all of them.
#include "generated/reflection_engine.c"

// windows.h defines these empty, which would silently erase any variable of that name in code after the engine.
#if OS_WINDOWS
#undef near
#undef far
#undef NEAR
#undef FAR
#endif
#endif
