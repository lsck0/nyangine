/**
 * @file build.h
 *
 * The one place the include order of the build system is written down.
 *
 *   flags.h        every name, path and compiler flag, and which of them this host uses
 *   hooks.h        the callbacks a rule hangs work off
 *   vendor/        one rule per third party dependency per target
 *   pp/            the generators that write C before anything compiles, and their rules
 *   on_linux/      the project rules that only a Linux host can produce
 *   build_windows  the project rules for Windows, native and cross compiled alike
 *   commands.h     the commands that are code rather than a rule
 *   cli.h          the command tree main dispatches on
 *   misc.h         the rules that are not the project: running, opening, the tool's own rebuild
 * */
#pragma once

#include "nyangine/nyangine.h"

// First, and it includes the host's toolchain: everything below expands its macros.
#include "build/flags.h"
/**/
#include "build/commands.h"
#include "build/hooks.h"
#include "build/vendor/vendor.h"
/**/
// After hooks.h and the vendors, both of which its rules name.
#include "build/pp/pp.h"
/**/
// The per host project rules. They depend on the asset rules in pp/pp.h, and flags.h has already
// aliased host_build_debug and friends to whichever of them this host defines.
#if !OS_WINDOWS
#include "build/on_linux/build_linux.h"
#endif
// Not per host: the native and cross compiled Windows rules were identical, so there is one copy.
// See build_windows.h.
#include "build/build_windows.h"
/**/
// Last: both name rules from everything above.
#include "build/cli.h"
#include "build/misc.h"
