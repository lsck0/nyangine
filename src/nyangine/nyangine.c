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
// Before either renderer and in both: it touches no GPU state, and a headless test reaches it.
//
// render_camera.c is here for a stronger reason than that — it is the camera arithmetic both
// renderers used to carry a copy of, and the copies had drifted. See its file comment.
#include "nyangine/renderer/render_camera.c"
// Shaping, and in both builds for the same reason: it is CPU only, so a headless build can
// and does lay text out exactly as the real one draws it. See render_text.h.
#include "nyangine/renderer/render_text.c"
// Where a shadow cascade goes, as opposed to how it is rasterised. Pure math, so both builds get it —
// see its file comment.
#include "nyangine/renderer/render_shadow.c"
#include "nyangine/renderer/render_sort.c"
#include "nyangine/renderer/render_lod.c"
#include "nyangine/renderer/render_occlusion.c"
#if NYA_HEADLESS_ENABLED
#include "nyangine/renderer/render2d_headless.c"
#else
#include "nyangine/renderer/render2d.c"
#endif
// After both renderers: particles draw through whichever the system is set to.
#include "nyangine/renderer/render_particles.c"
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
// After both 2D renderers: the post chain draws through whichever one the build selected.
#include "nyangine/renderer/render_post.c"
// After whichever 2D renderer was selected: it forwards to that renderer's text API.
#include "nyangine/renderer/render_font.c"
#include "nyangine/renderer/renderer.c"
// #include "nyangine/ui/ui.c"
#endif
