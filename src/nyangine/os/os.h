/**
 * @file os.h
 *
 * ── the os module ──
 *
 * os_file.h     files, directories and the paths the host names by itself
 * os_library.h  whether a shared library the machine provides can be loaded
 * os_page.h     virtual memory: reserve, commit, release, and what is resident
 * os_process.h  starting another program, its pipes, and waiting for it
 * os_random.h   the kernel's random source
 * os_socket.h   datagram and stream sockets, the wait over them, and addresses
 * os_thread.h   threads, mutexes and counting semaphores
 * os_time.h     the wall clock and the monotonic clock in nanoseconds, and waiting on them
 *
 * ── what belongs here ──
 *
 * One system call per function, and C types on both sides of it: no arena, no NYA_String, no
 * NYA_Error, no assertions. A function here says whether it worked and nothing more, because it is
 * below the code that knows what to do about it.
 *
 * That is the whole rule, and it is what lets `base` sit on top: the arena needs pages, the log needs a
 * clock and the build framework starts compilers, so whatever they call has to be below them. Anything
 * that wants an arena, allocates, formats, or decides what an error means belongs in `base`
 * (nya_filesystem_*, NYA_File, nya_command_*) or in `platform` (nya_clock_*, the terminal, ipc,
 * signals, the host's own description) instead.
 *
 * Every function here is implemented once per target in os_*_linux.c and os_*_windows.c, and those
 * are the only files in the engine that may hold a syscall. A target independent half belongs in the
 * caller, not here: see base/base_filesystem.c, which is the whole file system over these primitives
 * where it used to be ~30 functions written twice, base/base_command.c, which is the drain and the
 * capture written once, and base/base_clock.c, which is seven functions over two os calls where
 * it used to be seven functions twice.
 * */
#pragma once

#include "nyangine/os/os_file.h"
#include "nyangine/os/os_library.h"
#include "nyangine/os/os_page.h"
#include "nyangine/os/os_process.h"
#include "nyangine/os/os_random.h"
#include "nyangine/os/os_socket.h"
#include "nyangine/os/os_thread.h"
#include "nyangine/os/os_time.h"
