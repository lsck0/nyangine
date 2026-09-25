/**
 * @file nyangine.h
 * */

#pragma once

#include "nyangine-std/base/base.h"
#include "nyangine-std/os/os.h"
#include "nyangine-core/crypto/crypto.h"
#include "nyangine-std/math/math.h"
#include "nyangine-core/nn/nn.h"
#include "nyangine-std/platform/platform.h"
#include "nyangine-core/permission/permission.h"
#ifdef NYA_MODULE_DB
// Above base and crypto, below net, http and everything that stores anything. Behind a flag until
// the component system lands, because it wants sqlite on the include line and a host tool has none.
#include "nyangine-core/db/db.h"
// Above db, crypto and permission and below http: a user is a row, a password is a hash and a role is
// permission's bitmask. Inside db's flag because its two tables are db's.
#include "nyangine-core/accounts/accounts.h"
#endif
#include "nyangine-plugins/plugins.h"
#include "nyangine-std/serde/serde.h"
// Beside serde: both turn a NYA_Object into text, and neither includes the other. It only needs base,
// so it sits low and everything above it — http bodies, emails, LaTeX reports — can render through it.
#include "nyangine-core/template/template.h"
// Beside template: a command registry a dev console and a command palette dispatch through. Needs only
// base, so it sits low and is available to a headless server and a windowed game alike.
#include "nyangine-core/console/console.h"

// Above os and base and below http, which is the only thing here that wants a socket with a library
// on it. Its own header answers when the build has no TLS library at all.
#include "nyangine-core/tls/tls.h"

// Beside tls: an ACME client that gets and renews the certificate tls is handed. It signs with crypto and
// reaches the CA over a transport the program wires, so it needs no socket of its own; its OpenSSL half
// (the CSR) answers NYA_ERROR_NOT_SUPPORTED where there is no OpenSSL, the same as tls. A plugin now,
// opt-in behind NYA_PLUGIN_ACME (in the default plugin list), since a program that never renews a cert
// has no use for it.
#ifdef NYA_PLUGIN_ACME
#include "nyangine-plugins/acme/acme.h"
#endif

// net and http carry no include of core, the renderer or SDL (grep proves it), so a headless server
// takes them without the SDL half of the engine: NYA_SERVER is that build. Every existing build is
// unchanged — a full build (no NYA_NO_SDL) still gets them, a host tool (NYA_NO_SDL, no NYA_SERVER)
// still gets neither. See docs/layering-core-split.md; the socket transports are why a host tool that
// never opens one leaves them out.
#if !defined(NYA_NO_SDL) || defined(NYA_SERVER)
// Before net and http: net's encode/decode paths open trace scopes and http's DTOs are reflected. A
// headless build gets neither header through the SDL block below, so it takes them here — both are
// SDL-free: debug_trace.h names only base and compiles its scopes to no-ops outside a development build,
// and reflection_engine.h is a wall of extern declarations. Guarded to NYA_NO_SDL so a full build's
// include graph is unchanged — it still takes both from the block below, where their SDL-bound halves
// (debug_trace.c, the SDL reflection definitions) live. The server-safe reflection definitions are in
// reflection_engine_server.c, which nyangine.c compiles in this same seam.
#ifdef NYA_NO_SDL
#include "nyangine-core/debug/debug_trace.h"
#include "genyarated/reflection_engine.h"
#endif
#include "nyangine-core/net/net.h"
// Before core, which registers the drain as a frame system and whose metrics resource moved to debug:
// nothing under http names the app loop any more, which is what lets it come in without core here.
#include "nyangine-core/http/http.h"
#ifdef NYA_MODULE_DB
// After http and accounts both: the login flow as a mountable router. It is in http rather than accounts
// because it depends on both and http is the layer that may — accounts sits below it. Behind db's flag
// like accounts, whose primitives it wires; a server built without the db module leaves it out.
#include "nyangine-core/http/http_accounts.h"
#endif
#endif

#ifndef NYA_NO_SDL
// Before core, which names a body and a physics system in core_app.h, core_entity.h and
// core_world.h. Guarded with core rather than beside math because box2d and box3d are on the
// project include line and not on a host tool's; see physics.h.
#include "nyangine-core/physics/physics.h"
/**/
#include "nyangine-core/core/core.h"
// after core: a snapshot is captured out of the entity table and applied back into it, and the two
// ticks register themselves as engine systems. See replicate.h for why this is not part of net.
#include "nyangine-core/replicate/replicate.h"
#include "nyangine-core/renderer/render2d.h"
// after render2d.h, whose surface it replaces the implementation of. Empty unless -DNYA_TERMINAL.
#include "nyangine-core/renderer/render2d_terminal.h"
#include "nyangine-core/renderer/render2d_sprite.h"
#include "nyangine-core/renderer/render3d.h"
// before render_particles.h and render3d's foliage draw, both of which read a sampled wind vector. Pure
// CPU math, so a headless test samples it exactly as a shader would.
#include "nyangine-core/renderer/render_wind.h"
// beside the wind: the general analytic force field particles and fluids sample, a set of composable kinds
// (uniform, point, vortex, drag, turbulence). Before render_particles.h, which holds a NYA_ForceSet by pointer.
#include "nyangine-core/renderer/render_force.h"
// the flow-map and wave math a water surface rests on. Pure CPU math like the wind, and render3d.h's water
// draw path reads it, so it comes before renderer.h.
#include "nyangine-core/renderer/render_water.h"
#include "nyangine-core/renderer/render_particles.h"
// after render_particles.h and render_wind.h, both of which it is a thin skin over: rain and snow are
// particles carried by the shared wind field. Pure CPU math for the mode mapping, so a headless test reaches it.
#include "nyangine-core/renderer/render_weather.h"
// before renderer.h, which stores a window's NYA_FluidRenderOptions by value.
#include "nyangine-core/renderer/render_fluid.h"
#include "nyangine-core/debug/debug.h"
#include "nyangine-core/renderer/renderer.h"
// after renderer.h: the GPU compute pipeline skin and its particle-field effect. Desktop only — WebGL2 has
// no compute stage — so both are empty on the web build; see render_compute.h and the renderer-web wall.
#include "nyangine-core/renderer/render_compute.h"
#include "nyangine-core/renderer/render_compute_particles.h"
#include "nyangine-ui/ui.h"
#include "nyangine-ui/ui_present.h"
#include "nyangine-ui/ui_present_cell.h"
#include "nyangine-ui/ui_present_html.h"
#include "nyangine-ui/ui_present_dom.h"
// Only under NYA_TESTING: the simulation harness drives entities, physics, storage and the frame
// loop, so it has to see all of them.
#include "nyangine-core/testing/testing.h"

/*
 * Last, and inside the guard: it describes types declared above it, and the types it describes only
 * exist in an SDL build. See src/build/pp/reflection.h for why the engine has a table of its own.
 */
#include "genyarated/reflection_engine.h"
#endif
