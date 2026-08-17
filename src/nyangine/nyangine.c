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
#include "nyangine/renderer/renderer.c"
// #include "nyangine/ui/ui.c"
#endif
