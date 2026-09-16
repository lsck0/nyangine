/**
 * @file build.h
 * */
#pragma once

#include "nyangine/nyangine.h"

// Which host is doing the building decides the tool names every rule uses.
#include "build/toolchain.h"
/**/
#include "build/flags.h"
/**/
#include "build/bench.h"
#include "build/check.h"
#include "build/example.h"
#include "build/hooks.h"
#include "build/test.h"
#include "build/vendor/vendor.h"
/**/
// The preprocessor: everything that turns files on disk into generated C before anything compiles.
#include "build/pp/pp.h"
/**/
// The asset pipeline as build rules. After pp.h, whose functions the rules call through hooks.
#include "build/asset_rules.h"
/**/
// The per host project rules, and the macros that pick between them.
#include "build/host.h"
/**/
// The rules that are not the project, and the command line that runs all of it.
#include "build/cli.h"
#include "build/misc.h"
#include "build/rebuild.h"
