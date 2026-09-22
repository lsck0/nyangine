/**
 * @file debug.h
 * */
#pragma once

#include "nyangine/debug/debug_crash.h"
// the app serving its own numbers over HTTP reads the loop and the registries, which http, below core, must not.
#include "nyangine/debug/debug_metrics.h"
// drawing a network is debugging it, and needs the renderer that nn, a library above math only, must not.
#include "nyangine/debug/debug_nn.h"
#include "nyangine/debug/debug_nn_neat.h"
#include "nyangine/debug/debug_overlay.h"
#include "nyangine/debug/debug_physics.h"
#include "nyangine/debug/debug_trace.h"
