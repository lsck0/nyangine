/**
 * @file toolchain.h
 *
 * Selects the host's toolchain header.
 *
 * The per host files answer "what is doing the building", and which one applies is decided here so
 * that nothing else has to ask. Every vendor rule names macros from these files, and each of those
 * headers includes this one, which is what makes them compile on their own rather than only in
 * build.c's include order — the difference between clangd understanding a vendor file and clangd
 * reporting an undefined identifier in it.
 * */
#pragma once

#include "nyangine/base/base_basic.h"

#if OS_WINDOWS
#include "build/on_windows/toolchain.h"
#else
#include "build/on_linux/toolchain.h"
#endif
