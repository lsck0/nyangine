/**
 * @file build.h
 *
 * Everything the build system offers, in the order the pieces depend on each other.
 *
 * An umbrella and nothing else: every declaration below belongs to one of the headers it names, and
 * each of those includes what it uses rather than relying on this file having gone first. That is
 * what lets any of them be opened on its own — clangd compiles a header as its own translation
 * unit, so a file that only works in this file's include order reports undeclared identifiers in
 * the editor while building perfectly.
 *
 * toolchain.h comes first because it decides which host is doing the building, and every rule below
 * names the tools it defines. flags.h is next for the same reason: the vendors and the asset
 * pipeline both spell their compile flags with its macros.
 *
 * Mirrors nyangine.h, so build.c reads the same way main.c does: one header for the declarations,
 * one .c for the translation units behind them.
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
