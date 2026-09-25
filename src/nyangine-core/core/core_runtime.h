#pragma once

/*
 * The SDL-free floor of core.
 *
 * `core.h` is one module but two layers. Most of it names a window, a renderer, an asset on the GPU, a
 * physics body or an entity, and so cannot be read without the vendored SDL3/box2d headers on the include
 * line — that is why the whole of core, and http and net above it, sit inside `#ifndef NYA_NO_SDL` in
 * nyangine.h. But a real part of core needs none of that: the system registry, the callback table, the
 * job pool, the save root and the rest listed below reach for nothing but base, os, math, serde and
 * crypto. This header is that part named on its own, so that http, net and a headless server can one day
 * depend on the runtime core without dragging the renderer in behind it.
 *
 * The list is not a wish. Every header here was checked to pull zero SDL3 headers under `-DNYA_NO_SDL`
 * with no SDL include path — the include tree `clang -H` prints has no `SDL3/` line in it — while the
 * headers left in core.h (core_event.h's SDL_Mutex and event pump, core_asset.h and core_window.h's
 * SDL_GPU, core_input/settings/i18n/config reaching through them) each do. So this is the seam as it
 * stands today, not as it is wished to be: core_event.h is the linchpin that keeps events, input,
 * settings, i18n and config on the SDL side, and it belongs here the day its mutex becomes an NYA_Mutex
 * and its mouse-wheel enum stops borrowing SDL's. See docs/layering-core-split.md for the ordered plan
 * that moves the boundary down.
 *
 * This header adds a name; it removes nothing. core.h includes it and then includes everything else it
 * always did, so every existing build sees the identical set of declarations. Nothing yet compiles
 * against this header alone — that is the point of the next steps, not this one.
 */

// The registries and the loop's own vocabulary: a system, a callback token, a job, a task group, the
// frame simulation clock, and the entry-point contract a host selects a binary through. This is the part
// of the runtime a headless program actually reaches for.
#include "nyangine-core/core/core_types.h"
#include "nyangine-core/core/core_callback.h"
#include "nyangine-core/core/core_system.h"
#include "nyangine-core/core/core_sim.h"
#include "nyangine-core/core/core_job.h"
#include "nyangine-core/core/core_taskgroup.h"
#include "nyangine-core/core/core_app_entry.h"
#include "nyangine-core/core/core_http_reload.h"

// Persistence and its undo history: a save root is an arena written through serde, and an undo stack is a
// reflected diff over one. Neither touches a device.
#include "nyangine-core/core/core_save.h"
#include "nyangine-core/core/core_undo.h"

// The plugin registry and the signature check that gates it: a plugin is one entry in the system
// registry, and its signature is crypto over the bytes. Both sit above base and crypto and below SDL.
#include "nyangine-core/core/core_plugin.h"
#include "nyangine-core/core/core_plugin_signature.h"

// Interpolation and the skeleton math above it: tweens, the pose hierarchy, its inertial and blended
// variants and the layer mixer are all matrix and quaternion arithmetic a headless test steps exactly as
// a frame would.
#include "nyangine-core/core/core_tween.h"
#include "nyangine-core/core/core_skeleton.h"
#include "nyangine-core/core/core_skeleton_inertial.h"
#include "nyangine-core/core/core_skeleton_blend.h"
#include "nyangine-core/core/core_skeleton_layer.h"

// The audio model as declarations: a bus, its effect chain, the panner primitive and the propagation
// solver are vector math and typed config. The .c side calls SDL_mixer, but the types name none of it, so
// the headers belong on this side of the wall.
#include "nyangine-core/core/core_audio.h"
#include "nyangine-core/core/core_audio_panner.h"
#include "nyangine-core/core/core_audio_effects.h"
#include "nyangine-core/core/core_audio_propagation.h"

// Input vocabulary that is only names and codes: a keyboard scancode table and a gamepad's button and
// axis enums. The systems that read them (core_input, core_gamepad's .c) are SDL; these constants are
// not.
#include "nyangine-core/core/core_keys.h"
#include "nyangine-core/core/core_gamepad.h"
