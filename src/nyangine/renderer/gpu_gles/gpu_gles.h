/**
 * @file gpu_gles.h
 *
 * The public seam of the SDL_GPU → WebGL2 shim, and nothing else. See gpu_gles.c for the why.
 *
 * The whole file is `#if OS_WASM`: on every native target it is empty, so the engine and this header
 * both compile byte-for-byte as before and `./build check` never sees a line of it. The shim only
 * exists in the emscripten build, where no SDL library is linked and the SDL_GPU handle types are
 * ours to define (see gpu_gles.c's file comment).
 *
 * The two things a demo or a test needs from the shim that are not part of the SDL_GPU API itself:
 *   - whether a real WebGL2 context came up (it will not under node — there is no canvas), so the
 *     caller can decide between asserting real pixels and asserting the call sequence, and
 *   - a trace of the shim entry points, in call order, so a headless run can prove the 2D frame
 *     drove the shim in exactly the sequence SDL_GPU expects even when no GL ran.
 * */
#pragma once

#include "nyangine/base/base_basic.h"

#if OS_WASM

#include "nyangine/base/base_types.h"

// Not NYA_API: these are the shim's own diagnostics, used only by the wasm demo/test in the one
// unity translation unit that includes this shim (src/web/*.c, outside the engine's public surface and
// outside the lint roots). Plain prototypes, so the caller rule does not read them as unbacked API.

/** True once a WebGL2 context is current. False under node (no canvas) and before the device is made. */
b8 nya_gpu_gles_context_ok(void) __attr_no_discard;

/** Number of shim entry points recorded since the last reset. The trace is a fixed ring; it saturates. */
u32 nya_gpu_gles_trace_count(void) __attr_no_discard;

/** The name of the i-th recorded shim entry, or "" past the end. Order is call order. */
NYA_ConstCString nya_gpu_gles_trace_at(u32 index) __attr_no_discard;

/** Clears the trace, so a caller can bracket one frame and read exactly that frame's calls back. */
void nya_gpu_gles_trace_reset(void);

#endif // OS_WASM
