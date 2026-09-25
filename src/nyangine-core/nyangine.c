// first: base's arena takes its pages from it and its clock reads from it.
#include "nyangine-std/os/os.c"

#include "nyangine-std/base/base.c"
#include "nyangine-std/math/math.c"
#include "nyangine-core/nn/nn.c"
#include "nyangine-std/platform/platform.c"
#include "nyangine-std/serde/serde.c"
// Beside serde and above nothing but base: renders a NYA_Object to text. See nyangine.h.
#include "nyangine-core/template/template.c"
// Beside template and above nothing but base: the command registry. See nyangine.h.
#include "nyangine-core/console/console.c"
// Guarded: monocypher is on the project's include line and not the build tool's, which hashes nothing.
// Before the plugins, whose websocket handshake is SHA-1, and before net and http, which it serves.
// The seam is the header's, not plain NYA_NO_SDL: a headless server (NYA_SERVER) hashes sessions and
// signs plugins the same as a full build, so crypto's translation units come in for it too. A host
// build tool (NYA_NO_SDL, no NYA_SERVER) still gets none, exactly as before. See nyangine.h.
#if !defined(NYA_NO_SDL) || defined(NYA_SERVER)
#include "nyangine-core/crypto/crypto.c"
#endif
// below net and http, both of which ask it the same question a game asks it. No SDL and no sockets.
#include "nyangine-core/permission/permission.c"

// After crypto, which a keyed database will draw its key handling from, and before everything that
// stores anything. Behind its flag for the reason nyangine.h gives.
#ifdef NYA_MODULE_DB
#include "nyangine-core/db/db.c"
// after db, whose ORM holds its tables, and after crypto, which hashes its passwords and its tokens.
#include "nyangine-core/accounts/accounts.c"
#endif

// Each plugin is behind its own NYA_PLUGIN_* flag; see plugins.h.
#include "nyangine-core/plugins/plugins.c"

// tls, net, smtp, acme and http: the server half of the engine, and the whole of what a headless
// server links. The seam is the header's — a full build (no NYA_NO_SDL) and a headless server
// (NYA_SERVER) both compile these; a host build tool (NYA_NO_SDL, no NYA_SERVER) gets none, the same
// as before. This is the LINK half of the split the header opened: nyangine.h declares net and http
// behind this guard, and here is where their translation units are actually compiled for a server.
// See docs/layering-core-split.md, steps 6–7.
#if !defined(NYA_NO_SDL) || defined(NYA_SERVER)
// before http, whose listener wraps an accepted socket in a session. After os, whose descriptor it hands
// to OpenSSL, and after base, whose arena holds the context.
#include "nyangine-core/tls/tls.c"
#include "nyangine-core/net/net.c"
// after tls, whose client session it borrows for the encrypted link, and after os, whose socket it connects.
// A mail client, so it sits above the transport and below the HTTP server that has a reason to send mail.
#include "nyangine-core/smtp/smtp.c"
// beside smtp: an ACME client that signs with crypto and talks to the CA over a transport the program
// wires, so one binary renews its own certificate. Below http, whose route serves the challenge.
#include "nyangine-core/acme/acme.c"
#include "nyangine-core/http/http.c"
#ifdef NYA_MODULE_DB
// after http and accounts both: the login flow wired to HTTP. Not part of http's own umbrella, so plain
// http still builds without accounts; included here where both are present, headless server included.
#include "nyangine-core/http/http_accounts.c"
#endif
// After the server modules whose types it describes: the builtins and the reflection tables for every
// server-safe engine type (base, math, serde, net, http, and — behind NYA_MODULE_DB — db and accounts).
// The SDL-bound half is reflection_engine.c, in the block below; this is the half a headless build gets,
// and the one place the reflection builtins are defined so a full build has them without a duplicate.
#include "genyarated/reflection_engine_server.c"
#endif

#ifndef NYA_NO_SDL
// physics is box2d/box3d, so it stays on the SDL side with core, the renderer and the rest of the
// graph a windowed program links. A headless server has no bodies to step.
#include "nyangine-core/physics/physics.c"
// after http, which core_app.c drives once a frame; see nyangine.h.
#include "nyangine-core/core/core.c"
// after core, for the reason nyangine.h gives.
#include "nyangine-core/replicate/replicate.c"
// before either renderer and in both: no GPU state, and headless tests reach it. render_camera.c holds
// the camera arithmetic both renderers share.
#include "nyangine-core/renderer/render_camera.c"
// before either renderer: the wind field is analytic CPU math a headless test samples, and foliage in
// render3d.c reads a vector taken from it. See render_wind.h.
#include "nyangine-core/renderer/render_wind.c"
// beside the wind: the general force field is analytic CPU math a headless test samples, and the particle and
// fluid updates read it. Pure arithmetic plus the shared Perlin noise, so it belongs with the other CPU fields.
#include "nyangine-core/renderer/render_force.c"
// before either renderer, like the wind: the flow-map and wave math a water surface rests on is pure CPU
// arithmetic a headless test samples, and render3d.c pads a water cull radius by it. See render_water.h.
#include "nyangine-core/renderer/render_water.c"
// before everything that asks it what is switched on, and in both builds for the same reason.
#include "nyangine-core/renderer/render_features.c"
// both builds: asset loading creates textures through it, and headless tests read its counts.
#include "nyangine-core/renderer/render_gpu_memory.c"
// Shaping, and in both builds for the same reason: it is CPU only, so a headless build can
// and does lay text out exactly as the real one draws it. See render_text.h.
#include "nyangine-core/renderer/render_text.c"
// the CPU side of glyph atlases: cells and the coverage bake. Both builds, so a headless test reaches the bake.
#include "nyangine-core/renderer/render_glyph_atlas.c"
// where a shadow cascade goes, not how it is rasterised. Pure math, so both builds get it.
#include "nyangine-core/renderer/render_shadow.c"
// parsing a lookup table is CPU only, and the asset loader in both builds reaches it.
#include "nyangine-core/renderer/render_lut.c"
#include "nyangine-core/renderer/render_sort.c"
#include "nyangine-core/renderer/render_lod.c"
#include "nyangine-core/renderer/render_occlusion.c"
// after the occlusion buffer it tests against. CPU only, so headless tests reach the culling render3d.c runs.
#include "nyangine-core/renderer/render_cull.c"
// both builds: where a nine-slice's pieces go is arithmetic a headless test checks.
#include "nyangine-core/renderer/render_nine_slice.c"
// One 2D backend, picked at build time, so nothing carries another one's code. The terminal arm is
// checked first because NYA_TERMINAL implies NYA_HEADLESS; see base_basic.h.
#if NYA_TERMINAL_ENABLED
#include "nyangine-core/renderer/render2d_terminal.c"
#elif NYA_HEADLESS_ENABLED
#include "nyangine-core/renderer/render2d_headless.c"
#else
#include "nyangine-core/renderer/render2d.c"
#endif
// both builds: a veil of haze is a gradient rect, drawn through whichever renderer was picked.
#include "nyangine-core/renderer/render2d_haze.c"
// After both renderers: particles draw through whichever the system is set to.
#include "nyangine-core/renderer/render_particles.c"
// after render_particles.c: weather emits and draws through the particle system, and its mode mapping is
// pure CPU math a headless test steps without a GPU.
#include "nyangine-core/renderer/render_weather.c"
// After both renderers, for the reason particles are: a 2D volume draws through render2d and a 3D one
// through render3d, and the solver itself is CPU only, so a headless test steps and draws it.
#include "nyangine-core/renderer/render_fluid.c"
#include "nyangine-core/debug/debug.c"
#include "nyangine-core/renderer/render2d_sprite.c"
// After render2d.c: the real 3D flush reuses its pass suspend and resume, which are internal to that
// translation unit. Picked to match, so a headless build has no render pass on either side.
#if NYA_HEADLESS_ENABLED
#include "nyangine-core/renderer/render3d_headless.c"
#else
#include "nyangine-core/renderer/render3d.c"
#endif
// After render3d.c, whose flush draws them and whose internals they bind through.
#include "nyangine-core/renderer/render3d_decal.c"
#include "nyangine-core/renderer/render_output.c"
// After both 2D renderers: the post chain draws through whichever one the build selected.
#include "nyangine-core/renderer/render_post.c"
// After whichever 2D renderer was selected: it forwards to that renderer's text API.
#include "nyangine-core/renderer/render_font.c"
#include "nyangine-core/renderer/renderer.c"
#if !NYA_HEADLESS_ENABLED
#include "nyangine-core/renderer/render_trace.c"
// Real renderer only: the GPU compute pipeline skin and its particle-field proof call the SDL_GPU compute
// API, which the headless and terminal 2D backends do not carry. Desktop only within that — WebGL2 has no
// compute stage, so both are compiled out on the web build by the !OS_WASM guard in their own files.
#include "nyangine-core/renderer/render_compute.c"
#include "nyangine-core/renderer/render_compute_particles.c"
#include "nyangine-core/renderer/render_compute_volumetric.c"
#endif
// ui.c first: it defines the module's one static state, which every other ui_*.c file reads. The rest are
// independent of each other and only ordered to read alphabetically.
#include "nyangine-ui/ui.c"
// the seam, then its backends, then the widgets above them: nothing above ui_present_*.c names a primitive.
#include "nyangine-ui/ui_present.c"
#include "nyangine-ui/ui_present_record.c"
#include "nyangine-ui/ui_present_shape.c"
#include "nyangine-ui/ui_present_cell.c"
#include "nyangine-ui/ui_present_html.c"
#include "nyangine-ui/ui_present_dom.c"
#include "nyangine-ui/ui_code_editor.c"
#include "nyangine-ui/ui_draw.c"
#include "nyangine-ui/ui_input.c"
#include "nyangine-ui/ui_layout.c"
#include "nyangine-ui/ui_node.c"
#include "nyangine-ui/ui_style.c"
#include "nyangine-ui/ui_text.c"
#include "nyangine-ui/ui_widgets.c"
#include "nyangine-ui/ui_window.c"

// For the reason testing.h is included last.
#include "nyangine-core/testing/testing.c"

// Last: every type it describes has to be complete, and it names sizeof and offsetof on all of them.
#include "genyarated/reflection_engine.c"

// windows.h defines these empty, which would silently erase any variable of that name in code after the engine.
#if OS_WINDOWS
#undef near
#undef far
#undef NEAR
#undef FAR
#endif
#endif
