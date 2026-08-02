/**
 * @file nyangine.h
 *
 * Compilation options:
 * -DVERSION="<version_string>" : Set the engine version string.
 * -DGIT_COMMIT="<git_commit_hash>" : Set the git commit hash.
 * -DNYA_EXECUTION_MODE=<mode> : Set the execution mode (0 debug, 1 developer, 2 release, 3 steam).
 * -DNYA_NO_ASSERT : Disable runtime assertions. Does not affect nya_assert_always.
 * -DNYA_TESTING : Enable crash prevention (nya_expect_crash). TEST BUILDS ONLY, never ship this.
 * -DNYA_ARENA_FORCE_DEBUG : Force arena to use debug mode.
 * -DNYA_ARENA_FORCE_NODEBUG : Force arena to use no-debug mode.
 * -DNYA_ASSET_BACKEND_FS : Use filesystem as the asset backend.
 * -DNYA_ASSET_BACKEND_BLOB : Use embedded blob as the asset backend.
 * -DNYA_ASSET_HOT_RELOAD : Enable asset hot-reloading, only works with filesystem backend.
 * -DNYA_HEADLESS : Run without drawing anything. No GPU device is created and every render call
 *                  becomes a no-op, while events, jobs, assets and the simulation still run. For
 *                  tests and CI machines that have no device to create. Unlike NYA_NO_SDL the
 *                  renderer is still compiled and callable.
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
#include "nyangine/serde/serde.h"
#include "nyangine/steam/steam.h"

#ifndef NYA_NO_SDL
#include "nyangine/core/core.h"
#include "nyangine/renderer/renderer.h"
#include "nyangine/ui/ui.h"
#endif
