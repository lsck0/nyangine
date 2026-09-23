/**
 * @file nyangine.h
 * */

#pragma once

#include "nyangine/base/base.h"
#include "nyangine/os/os.h"
#include "nyangine/crypto/crypto.h"
#include "nyangine/math/math.h"
#include "nyangine/nn/nn.h"
#include "nyangine/platform/platform.h"
#include "nyangine/permission/permission.h"
#ifdef NYA_MODULE_DB
// Above base and crypto, below net, http and everything that stores anything. Behind a flag until
// the component system lands, because it wants sqlite on the include line and a host tool has none.
#include "nyangine/db/db.h"
#endif
#include "nyangine/plugins/plugins.h"
#include "nyangine/serde/serde.h"

#ifndef NYA_NO_SDL
// Before core, which names a body and a physics system in core_app.h, core_entity.h and
// core_world.h. Guarded with core rather than beside math because box2d and box3d are on the
// project include line and not on a host tool's; see physics.h.
#include "nyangine/physics/physics.h"
// Guarded for the same reason: the transports link SDL_net, which a host tool does not have.
#include "nyangine/net/net.h"
// Before core, which registers the drain as a frame system and whose metrics resource moved to debug:
// nothing under http names the app loop any more.
#include "nyangine/http/http.h"
/**/
#include "nyangine/core/core.h"
// after core: a snapshot is captured out of the entity table and applied back into it, and the two
// ticks register themselves as engine systems. See replicate.h for why this is not part of net.
#include "nyangine/replicate/replicate.h"
#include "nyangine/renderer/render2d.h"
// after render2d.h, whose surface it replaces the implementation of. Empty unless -DNYA_TERMINAL.
#include "nyangine/renderer/render2d_terminal.h"
#include "nyangine/renderer/render2d_sprite.h"
#include "nyangine/renderer/render3d.h"
#include "nyangine/renderer/render_particles.h"
// before renderer.h, which stores a window's NYA_FluidRenderOptions by value.
#include "nyangine/renderer/render_fluid.h"
#include "nyangine/debug/debug.h"
#include "nyangine/renderer/renderer.h"
#include "nyangine/ui/ui.h"
#include "nyangine/ui/ui_present.h"
#include "nyangine/ui/ui_present_cell.h"
// Only under NYA_TESTING: the simulation harness drives entities, physics, storage and the frame
// loop, so it has to see all of them.
#include "nyangine/testing/testing.h"

/*
 * Last, and inside the guard: it describes types declared above it, and the types it describes only
 * exist in an SDL build. See src/build/pp/reflection.h for why the engine has a table of its own.
 */
#include "genyarated/reflection_engine.h"
#endif
