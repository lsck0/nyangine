/**
 * @file base_lambda.h
 *
 * A callback written where it is handed over rather than three hundred lines away.
 *
 * ```c
 * // in layer_pause_menu.c, which includes "genyarated/lambdas/gnyame_layers_layer_pause_menu_c.h"
 * nya_sim_defer(nya_lambda(gny_locale_apply, void, (void* data), {
 *     u32 index = *(u32*)data;
 *     NYA_Error loaded = nya_i18n_load(_GNY_LOCALES[index].locale, NYA_STRING_KEYS, NYA_STRING_COUNT);
 *     if (!loaded.ok) nya_log_warn("Could not load that locale: %s", (NYA_ConstCString)loaded.message);
 * }), &index, sizeof(index));
 * ```
 *
 * `src/build/pp/lambda.c` reads every call site before anything compiles and writes each body out as a
 * real function in a companion header, one per source file, which that file includes itself. The macro
 * expands to that function's name and drops everything else, so what is left at the call site is a
 * plain function pointer: no closure, no allocation, no indirection, nothing to free.
 *
 * ## It cannot capture, and that is the point
 *
 * The hoisted function sits at file scope, so naming a local of the enclosing function inside the body
 * is an ordinary compile error rather than a pointer into a frame that has returned. A callback here
 * outlives the expression that created it — it is stored in a registry, queued until the barrier, or
 * called by another thread — and a capturing lambda would hand all three a dangling reference. What the
 * body needs it takes as a parameter or through the `void*` the callback API already carries, which is
 * the same discipline every other callback in this tree follows.
 *
 * ## What else it costs
 *
 * The body sees what is in scope where the companion is included, which is normally right after the
 * file's private declarations, and nothing that is declared below that line. The tag is one name across
 * the whole tree, because it is one function's name; the pass says which two call sites collided.
 *
 * The pass reads text and does not evaluate `#if`, so a lambda inside an arm that is compiled out is
 * still hoisted, and is then a function nothing calls, which `-Werror` refuses. Write an ordinary one
 * there.
 * */
#pragma once

// FUNCTIONS AND MACROS

/**
 * `nya_lambda(tag, ReturnType, (parameters), { body })`: the name of the function the body became.
 *
 * Variadic so the body's commas belong to the body. Everything past the tag is read by the pass and
 * dropped here, which is why a body that changed still has to be regenerated to change anything.
 * */
#define nya_lambda(tag, ...) _nya_lambda_##tag
