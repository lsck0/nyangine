/**
 * @file debug.h
 *
 * Developer-facing drawing tools; a game opts in by calling them, the engine never calls them
 * itself. Depends on the renderer, so this module is excluded from -DNYA_NO_SDL builds.
 * */
#pragma once

#include "nyangine/debug/debug_overlay.h"
