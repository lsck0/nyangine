/**
 * @file core_app_entry.h
 *
 * The contract between the generic hot-reload host (src/main.c) and an app.
 *
 * A project builds many app binaries — gnyame-client, gnyame-server, gnyame-cli — and one host loads
 * whichever it was pointed at. So the host cannot know an app's own names: it resolves this fixed trio
 * out of the DLL by symbol and calls nothing else. Every app DLL exports it, and a release build links
 * one app in and calls the same three directly. The names are the seam; the app behind them is a build
 * decision, not the host's.
 *
 * These are the DLL boundary, not the engine's own lifecycle. `nya_app_init`/`nya_app_run`/
 * `nya_app_deinit` (core_app.h) bring the engine up, spin the frame loop and tear it down; the trio
 * here is what a program wraps around them, one layer out, and is where the host enters.
 *
 * ## What each one owes the host
 *
 * `nya_app_entry_init(argc, argv)` reads the command line and brings up the app's parts. It returns
 * false when there is nothing to run — `--help`, or a line that could not be parsed — and then nothing
 * was started and `nya_app_entry_deinit` must not be called either. It must have called `nya_app_init`
 * by the time it returns true, since the host reads `nya_app_get()` straight after.
 *
 * `nya_app_entry_run()` runs the app until `nya_app_get()->should_quit`, then returns. The host calls
 * it in a loop: a code reload sets `should_quit`, the call returns, the host swaps the DLL and calls it
 * again, so a run that must resume after a reload keeps its state in the engine world, not in globals.
 *
 * `nya_app_entry_deinit()` saves and takes the app back down.
 * */
#pragma once

#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

/**
 * Reads the command line and brings up the parts it asks for. False when there is nothing to run, and
 * then nothing was brought up and nya_app_entry_deinit must not be called. See the file comment.
 * */
NYA_API b8 nya_app_entry_init(s32 argc, NYA_CString* argv);

/** Runs the app until should_quit, then returns. Re-entered after a code reload. */
NYA_API void nya_app_entry_run(void);

/** Saves and takes the app down. */
NYA_API void nya_app_entry_deinit(void);
