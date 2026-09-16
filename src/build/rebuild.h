/**
 * @file rebuild.h
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
        "-fsanitize-ignorelist=src/build/sanitizer_ignorelist.txt",
    },
};
