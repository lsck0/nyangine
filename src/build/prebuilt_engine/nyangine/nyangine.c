/**
 * @file nyangine.c
 *
 * What a test's `#include "nyangine/nyangine.c"` resolves to when the test links the engine object the
 * test runner compiles once, instead of compiling the engine again. Declarations only; see test.c.
 * */
#pragma once

// headers declare a few internal functions that only the engine's own sources define and call.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "nyangine/nyangine.h"
#pragma clang diagnostic pop
