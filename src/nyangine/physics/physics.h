/**
 * @file physics.h
 *
 * Rigid body simulation: Box2D behind physics2d.h, Box3D behind physics3d.h, and the vocabulary
 * they share in physics_types.h.
 *
 * ## Why this is not part of core
 *
 * It was, as core_physics*.h, and the arrangement claimed something untrue: that simulating bodies
 * is as fundamental to the engine as the frame loop, the event queue and the asset system. It is
 * not. A game can run without a solver, two thirds of the engine never mentions one, and the two
 * solvers are the only place in core that pulled in a third party library with a world of its own.
 *
 * The dependency runs one way at the header level, which is what makes the split hold: these
 * headers need nothing from core but the handle types in core_types.h, while core_entity.h,
 * core_world.h and core_app.h all name a body or a physics system. Core depends on physics; physics
 * does not depend on core.
 *
 * The *implementations* do reach back into core — a contact has to resolve an entity handle and
 * call its on_collision — but that is a unity build detail rather than a layering one: physics2d.c
 * includes nyangine.h like every other translation unit and sees the whole engine. Splitting the
 * engine into real objects is what would make that a problem, and nothing here is closer to that
 * than it was in core.
 *
 * ## Still behind NYA_NO_SDL
 *
 * Not because a solver needs SDL — neither does — but because the include line for box2d and box3d
 * comes from the project's vendor list, and a host tool built with NYA_NO_SDL is compiled without
 * it. The build system itself is exactly that tool. See nyangine.h.
 * */
#pragma once

#include "nyangine/physics/physics_types.h"
/**/
#include "nyangine/physics/physics2d.h"
#include "nyangine/physics/physics3d.h"
