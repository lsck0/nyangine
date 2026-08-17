/**
 * @file rebuild.h
 *
 * How the build tool recompiles itself.
 *
 * The one rule that is not about the project: its source is build.c, its output is the binary
 * currently running, and main hands it to nya_rebuild_yourself before doing anything else. Editing
 * any of the build system and re-running ./build is what that buys — the tool notices it is stale,
 * compiles over itself, and exec's the new one with the same arguments.
 *
 * On its own rather than in misc.h because the constraints on it are unlike anything there. The
 * output name has to be exactly what the running executable is called, the flags come from the host
 * block rather than any target's, and main mutates the argument list at runtime. All three are
 * spelled out below.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/flags.h"
// For BUILD_TOOL_BINARY and FLAGS_HOST_NATIVE.
#include "build/host.h"
// For CC.
#include "build/vendor/vendor_common.h"

/**
 * Builds the host's own build tool.
 *
 * Every flag that names a platform comes from the host block in build.h rather than from a target's
 * set: FLAGS_LINUX_X86_64 is an ELF rpath and FLAGS_WINDOWS_X86_64 carries -Wl,-subsystem,windows,
 * which would turn this command line tool into a GUI process with nowhere to print.
 *
 * BUILD_TOOL_BINARY has to be exactly what the running executable is called. nya_rebuild_yourself
 * backs up argv[0], compiles over it, then exec's it again; name the output anything else and the
 * compile writes a file nobody runs while the stale binary re-execs itself, which looks like a
 * build system that silently ignores its own source changes.
 *
 * Deliberately not const: main appends libbacktrace's include paths and archive to `arguments` once
 * that archive exists on disk, which on a fresh checkout it does not. See the note there.
 * */
NYA_INTERNAL NYA_Command build_rebuild_command = {
    .program   = CC,
    .arguments = {
        "build.c",
        "-o", BUILD_TOOL_BINARY,
        CFLAGS,
        WARNINGS,
        INCLUDE_PATHS,
        LINKER_FLAGS,
        FLAGS_DEBUG,
        FLAGS_HOST_NATIVE,
        FLAGS_BUILD_TOOL,
    },
};
