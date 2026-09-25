/**
 * @file base_preflight.h
 *
 * Declaring a runtime dependency a build takes from the machine, so a missing one is a descriptive
 * crash at startup rather than an obscure failure in the middle of the operation that needed it.
 *
 * ```c
 * // a program whose login requires the PGP second factor, at startup:
 * nya_require_program("gpg", "the PGP second-factor login flow");
 *
 * // a subsystem that dlopens a machine's OpenSSL, before it does:
 * nya_require_library("libssl.so.3", "the TLS server listener");
 * ```
 *
 * ── what this is for ──
 *
 * Most of what a nyangine program needs is linked into it: the vendored libraries under vendor/ are
 * static archives, part of the executable, and cannot go missing. A few things genuinely are not —
 * `gpg`, spawned by the PGP plugin; OpenSSL on Linux, which tls.h takes from the machine on purpose so
 * the machine's own updates fix it. Those are *runtime* dependencies: the build says nothing about
 * them, and whether they are here is only known when the program runs on a particular box.
 *
 * The failure this replaces is the quiet one: a container nobody put gpg in starts fine, serves for an
 * hour, and then the first user to reach for the PGP factor gets an internal error, and the log says
 * "exit code 127" a layer down from anything that names gpg. A program that knows it needs gpg says so
 * at startup with nya_require_program, and a box without it never gets past launch — with a message
 * that names the dependency, the feature that wanted it, and that it was not found.
 *
 * ── opt in, per program ──
 *
 * The engine core requires none of this. A dependency is declared by the program or subsystem that
 * actually needs it, at its own startup, because a tool one program spawns is not a tool every program
 * should refuse to launch without. The PGP plugin exposes nya_pgp_require for exactly this: it is not
 * called from anywhere in the engine, only from a program that has decided the feature is mandatory.
 *
 * ── the split ──
 *
 * The present/absent question and the crash are two functions. nya_preflight_program_present and
 * nya_preflight_library_present answer the question and return, so a test can drive them on a real
 * name and a bogus one without dying; nya_require_program and nya_require_library are the thin abort
 * over them that a startup path calls. What they crash through is nya_log_panic, so the message lands
 * in the crash report and every log sink like any other panic.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// PRESENT / ABSENT

/**
 * Whether `name` is a program that could be run, resolved the way a spawn would resolve it.
 *
 * A bare name is looked up on PATH; a name with a directory in it is checked as it stands. Answers and
 * returns — this is the testable core, and it never crashes. False when it is on no PATH entry or
 * `name` is null or empty.
 * */
NYA_API b8 nya_preflight_program_present(NYA_ConstCString name) __attr_no_discard;

/**
 * Whether the shared library `soname` can be loaded right now, without keeping it loaded.
 *
 * The loader's own search, closed again at once. Answers and returns; the testable core of the library
 * requirement. False when it is on no search path or `soname` is null or empty.
 * */
NYA_API b8 nya_preflight_library_present(NYA_ConstCString soname) __attr_no_discard;

// REQUIRE (CRASH WHEN ABSENT)

/**
 * Crashes now, naming the dependency, unless `name` is a program on PATH.
 *
 * `why` is what needs it, in a few words, and goes into the message so whoever reads the crash knows
 * which feature the missing program was for: "the PGP second-factor login flow", say. Returns quietly
 * when the program is present, so a startup path can call it unconditionally.
 * */
NYA_API void nya_require_program(NYA_ConstCString name, NYA_ConstCString why);

/**
 * Crashes now, naming the dependency, unless `soname` is a shared library the loader can load.
 *
 * The counterpart to nya_require_program for a library a build takes from the machine rather than
 * vendoring. `why` is the feature that needs it. Returns quietly when it is present.
 * */
NYA_API void nya_require_library(NYA_ConstCString soname, NYA_ConstCString why);
