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
// after core: the server registers its drain on the frame's own event and its metrics resource reads
// the app, the ceiling registry and the system registry.
#include "nyangine/http/http.h"
#include "nyangine/renderer/render2d.h"
// after render2d.h, whose surface it replaces the implementation of. Empty unless -DNYA_TERMINAL.
#include "nyangine/renderer/render2d_terminal.h"
#include "nyangine/renderer/render2d_sprite.h"
#include "nyangine/renderer/render3d.h"
#include "nyangine/renderer/render_particles.h"
// before renderer.h, which stores a window's NYA_FluidRenderOptions by value.
#include "nyangine/renderer/render_fluid.h"
// the NEAT visualizer needs the renderer, so it sits with it. The build tool compiles nn with
// -DNYA_NO_SDL.
#include "nyangine/nn/nn_draw.h"
#include "nyangine/nn/nn_neat_draw.h"
#include "nyangine/debug/debug.h"
#include "nyangine/renderer/renderer.h"
#include "nyangine/ui/ui.h"
// Only under NYA_TESTING: the simulation harness drives entities, physics, storage and the frame
// loop, so it has to see all of them.
#include "nyangine/testing/testing.h"

/*
 * Last, and inside the guard: it describes types declared above it, and the types it describes only
 * exist in an SDL build. See src/build/pp/reflection.h for why the engine has a table of its own.
 */
#include "generated/reflection_engine.h"
#endif
