/**
 * @file nyangine.h
 *
 * Compilation options:
 * -DDEBUG=<true/false> : Compile in debug mode.
 * -DVERSION="<version_string>" : Set the engine version string.
 * -DGIT_COMMIT="<git_commit_hash>" : Set the git commit hash.
 * -DNYA_NO_ASSERT : Disable runtime assertions.
 * -DNYA_ARENA_FORCE_DEBUG : Force arena to use debug mode, to enable it in release builds.
 * -DNYA_ARENA_FORCE_NODEBUG : Force arena to use no-debug mode, to disable it in debug builds.
 * -DNYA_ASSET_BACKEND_FS : Use filesystem as the asset backend.
 * -DNYA_ASSET_BACKEND_BLOB : Use embedded blob as the asset backend.
 * -DNYA_ASSET_HOT_RELOAD : Enable asset hot-reloading, only works with filesystem backend.
 * */

#pragma once

#include "nyangine/base/base.h"
#include "nyangine/core/core.h"
#include "nyangine/math/math.h"
#include "nyangine/nn/nn.h"
#include "nyangine/platform/platform.h"
#include "nyangine/renderer/renderer.h"
#include "nyangine/steam/steam.h"
#include "nyangine/ui/ui.h"
