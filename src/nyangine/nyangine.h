/**
 * @file nyangine.h
 *
 * Compilation options:
 * -DVERSION="<version_string>" : Set the engine version string.
 * -DGIT_COMMIT="<git_commit_hash>" : Set the git commit hash.
 * -DNYA_EXECUTION_MODE=<mode> : Set the execution mode (0 debug, 1 developer, 2 release, 3 steam).
 *   (There is no flag to disable assertions. -DNYA_NO_ASSERT is refused with an #error; see
 *   base_basic.h for why they stay on in shipping builds.)
 * -DNYA_TESTING : Enable crash prevention (nya_expect_crash). TEST BUILDS ONLY, never ship this.
 * -DNYA_ARENA_FORCE_DEBUG : Force arena to use debug mode.
 * -DNYA_ARENA_FORCE_NODEBUG : Force arena to use no-debug mode.
 * -DNYA_ASSET_PREFER_BLOB : Bake src/generated/assets.c into the binary and look there before the
 *                    filesystem. Anything the blob does not carry still comes off disk, so this is a
 *                    cache in front of the filesystem rather than a replacement for it.
 *                    (Listed here as -DNYA_ASSET_BLOB until now, which is not the macro any #ifdef
 *                    tests — passing that name built a release with no blob and no diagnostic.)
 * -DNYA_ASSET_HOT_RELOAD : Watch assets that came off disk and reload them when they change. An
 *                    asset served out of the blob has no file behind it and is never watched, so
 *                    this is independent of NYA_ASSET_PREFER_BLOB and the two combine.
 * The filesystem is always available; neither flag is required.
 * -DNYA_HEADLESS : Run without drawing anything. No GPU device is created and every render call
 *                  becomes a no-op, while events, jobs, assets and the simulation still run. For
 *                  tests and CI machines that have no device to create. Unlike NYA_NO_SDL the
 *                  renderer is still compiled and callable.
 * -DNYA_PLUGIN_CURL : Compile plugins/curl, JSON REST over libcurl. Off by default; see plugins.h.
 * -DNYA_PLUGIN_SQLITE : Compile plugins/sqlite, SQLite rows as NYA_Object. Off by default.
 * -DNYA_PLUGIN_DISCORD : Compile plugins/discord, Discord Rich Presence over the local IPC
 *                  socket. Off by default. Needs no library and no vendored SDK; see
 *                  plugins/discord/discord.h for why it is not the Discord Social SDK.
 * -DNYA_PLUGIN_STEAM : Compile plugins/steam, the Steamworks flat API. Off by default, and it
 *                  needs the steam_api redistributable on the link line and beside the binary;
 *                  see plugins/steam/steam.h.
 * -DNYA_NO_SDL : Exclude every module that links against SDL, namely core, renderer and ui.
 *                Host side tools such as the build system want this: they need base, math,
 *                platform and serde, and linking SDL into them would be dead weight at best and
 *                an unnecessary build dependency at worst.
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
