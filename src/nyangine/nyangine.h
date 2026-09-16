/**
 * @file nyangine.h
 * */

#pragma once

#include "nyangine/base/base.h"
#include "nyangine/math/math.h"
#include "nyangine/nn/nn.h"
#include "nyangine/platform/platform.h"
#include "nyangine/plugins/plugins.h"
#include "nyangine/serde/serde.h"

#ifndef NYA_NO_SDL
// Before core, which names a body and a physics system in core_app.h, core_entity.h and
// core_world.h. Guarded with core rather than beside math because box2d and box3d are on the
// project include line and not on a host tool's; see physics.h.
#include "nyangine/physics/physics.h"
// Guarded for the same reason: the transports link SDL_net, which a host tool does not have.
#include "nyangine/net/net.h"
/**/
#include "nyangine/core/core.h"
#include "nyangine/renderer/render2d.h"
#include "nyangine/renderer/render2d_sprite.h"
#include "nyangine/renderer/render3d.h"
#include "nyangine/renderer/render_particles.h"
// The NEAT visualizer needs the renderer, unlike the rest of nn, so it sits with it rather than
// with the algorithm — the build tool compiles nn with -DNYA_NO_SDL and has no renderer at all.
#include "nyangine/nn/nn_draw.h"
#include "nyangine/nn/nn_neat_draw.h"
#include "nyangine/debug/debug.h"
#include "nyangine/renderer/renderer.h"
#include "nyangine/ui/ui.h"
#endif
