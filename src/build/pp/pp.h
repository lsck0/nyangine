/**
 * @file pp/pp.h
 *
 * The preprocessor: the passes that turn files on disk into C source before anything compiles.
 *
 * Three of them, and they all have the same shape — read a tree, write a generated .c or .h into
 * src/generated/:
 *
 * - asset.h      walks ./assets, writes the handle table and the embedded blob, and compiles shaders
 * - i18n.h       reads the locale files under assets/i18n, validates them against the base locale
 * - reflection.h scans the source tree for @reflect annotations, writes the type tables
 *
 * All three open by asking stale.h whether anything they read has moved since they last wrote, and
 * return without doing anything when it has not. Their rules are NYA_BUILD_ALWAYS and stay that
 * way: the build system compares one input file to one output file, and none of these passes has
 * that shape.
 *
 * None of them knows what an NYA_BuildRule is. Scheduling belongs to build/asset_rules.h, which is
 * the difference this directory exists to draw: these are the passes, that is when they run.
 * */
#pragma once

// First: every pass below opens by calling it.
#include "build/pp/stale.h"
/**/
#include "build/pp/asset.h"
#include "build/pp/i18n.h"
#include "build/pp/reflection.h"
