/**
 * @file luabind.h
 *
 * The Lua bindings, read out of the engine headers rather than written by hand.
 *
 * A public declaration with a `@lua` annotation becomes three things on the next build: a marshalling
 * function that takes the call's NYA_Values apart and puts the C function's answer back, a row in the
 * table nya_lua_open_engine_permitted walks, and an entry in a definitions file an editor reads. The
 * permission the call needs is the annotation's argument, so it lives above the C function rather than
 * in a list somebody has to remember to update.
 *
 * A doc comment reading "Whether the action is held down right now." with an `@lua(INPUT)` line in it,
 * above:
 *
 * ```c
 * NYA_API b8 nya_input_action_pressed(NYA_InputAction action);
 * ```
 *
 * becomes `nya.input.action_pressed(action)` in Lua: the `nya_` prefix comes off, the first word is
 * the sub-table and the rest is the function. `@lua(INPUT, some.other.path)` overrides that where the
 * derived name reads badly.
 *
 * ─────────────────────────────────────────────────────────
 * WHAT IT WILL NOT GENERATE
 * ─────────────────────────────────────────────────────────
 *
 * Stated here rather than discovered as a binding that quietly never appeared. Each of these is
 * reported on the build's output, naming the declaration:
 *
 * - **A type it has no marshalling for.** Numbers, `b8`, `NYA_ConstCString`, `NYA_EntityHandle` and
 *   plain enums cross; a struct, a pointer, an array or a callback does not. A pointer especially:
 *   handing a script an address is the thing this boundary exists to prevent.
 * - **Variadic functions.** `nya_log_info(format, ...)` has no signature to generate against.
 * - **Overloaded functions.** Two C functions of one name are two Lua functions of one name, and
 *   picking between them at runtime would mean guessing from the argument types.
 * - **Out parameters.** A script has nothing to pass by address. A call that answers through one needs
 *   a wrapper that returns instead, and that wrapper is the thing to annotate.
 *
 * What cannot be generated is written by hand, annotated `@lua_manual(path, PERMISSION, signature)`
 * and registered by whoever wrote it. The annotation is there so the definitions file still describes
 * it: an editor must see one surface, not the generated half of one.
 * */
#pragma once

#include "nyangine/nyangine.h"

/* CONSTANTS */

/** The tree scanned for annotations. Headers for `@lua`, sources for `@lua_manual`. */
#define NYA_LUABIND_DIRECTORY "./src/nyangine"

/** The generated C: one marshalling function per binding, and the table they are registered from. */
#define NYA_LUABIND_OUTPUT_SOURCE "./src/genyarated/lua_bindings.c"

/**
 * The generated Lua. A `---@meta` definitions file, which is what stops an editor calling `nya` an
 * undefined global. Point a language server at the directory; `.luarc.json` at the repository root
 * already does.
 * */
#define NYA_LUABIND_OUTPUT_DEFINITIONS "./docs/lua/nya.lua"

/** The markers. Anywhere at the start of a line inside a comment directly above the declaration. */
#define NYA_LUABIND_MARKER        "@lua("
#define NYA_LUABIND_MARKER_MANUAL "@lua_manual("

/**
 * Bindings one build may generate. The engine exports about nine hundred public functions and this
 * annotates the few dozen a script has any business calling; 256 is the ceiling the VM registers into
 * (NYA_LUA_MAX_BINDINGS) and going past it would fail at runtime instead of here.
 * */
#define NYA_LUABIND_MAX_BINDINGS 256

/** Parameters one bound function may take. Past three the C call wants an options struct anyway. */
#define NYA_LUABIND_MAX_PARAMETERS 8

/** Longest source line held while joining a declaration, and longest joined declaration. */
#define NYA_LUABIND_MAX_LINE  1024
#define NYA_LUABIND_MAX_ENTRY 4096

/** Longest identifier, dotted path or rendered Lua signature. */
#define NYA_LUABIND_MAX_NAME 128

/** A doc comment's first sentence is carried over only when it fits whole; a longer one is left out. */
#define NYA_LUABIND_MAX_SUMMARY 140

/* FUNCTIONS */

/**
 * Scans the tree and writes both outputs. Stale checked, so it costs nothing when no header moved.
 * */
void nya_luabind_generate(void);
