/**
 * @file debug.h
 *
 * Tools that exist to explain the engine to a developer rather than to run the game.
 *
 * Everything here draws, so the whole module is excluded from -DNYA_NO_SDL builds along with the
 * renderer it depends on — the build tool compiles the engine without SDL and has nothing to draw
 * into.
 *
 * Nothing in here is called by the engine itself. A game opts in by calling it, which is what keeps
 * it free when it is off.
 * */
#pragma once

#include "nyangine/debug/debug_overlay.h"
