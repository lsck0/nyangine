/**
 * @file os.h
 *
 * ── the os module ──
 *
 * os_page.h     virtual memory: reserve, commit, release, and what is resident
 * os_random.h   the kernel's random source
 * os_time.h     the wall clock and the monotonic clock, in nanoseconds
 *
 * ── what belongs here ──
 *
 * One system call per function, and C types on both sides of it: no arena, no NYA_String, no
 * NYA_Error, no assertions. A function here says whether it worked and nothing more, because it is
 * below the code that knows what to do about it.
 *
 * That is the whole rule, and it is what lets `base` sit on top: the arena needs pages and the log
 * needs a clock, so whatever they call has to be below them. Anything that wants an arena, allocates,
 * formats, or decides what an error means belongs in `base` (nya_filesystem_*, nya_clock_*) or in
 * `platform` (the terminal, ipc, signals, the host's own description) instead.
 *
 * Every function here is implemented once per target in os_*_linux.c and os_*_windows.c, and those
 * are the only files in the engine that may hold a syscall. A target independent half belongs in the
 * caller, not here: see platform/clock/clock.c, which is seven functions over two os calls where it
 * used to be seven functions twice.
 * */
#pragma once

#include "nyangine/os/os_page.h"
#include "nyangine/os/os_random.h"
#include "nyangine/os/os_time.h"
