/**
 * @file os_process.h
 *
 * Starting another program, watching it, and reading what it writes.
 *
 * ```c
 * NYA_OsPipe read_end  = NYA_OS_PIPE_NONE;
 * NYA_OsPipe write_end = NYA_OS_PIPE_NONE;
 * if (nya_os_pipe_open(&read_end, &write_end) != NYA_OS_PROCESS_OK) return false;
 *
 * NYA_OsProcess      child = { 0 };
 * NYA_OsProcessSpawn spawn = {
 *     .program     = "cc",
 *     .arguments   = (NYA_ConstCString[]){ "cc", "--version", nullptr },
 *     .stdout_pipe = write_end,
 *     .stderr_pipe = write_end,
 * };
 * NYA_OsProcessStatus status = nya_os_process_spawn(&spawn, &child);
 * nya_os_pipe_close(write_end);   // the child holds its own copy now
 * ```
 *
 * The primitives and nothing above them: no argument quoting policy, no capture buffers, no timings.
 * Draining two pipes without deadlocking a child that fills one of them is logic rather than a system
 * call, so it lives once in base_command.h, which is what the rest of the engine spawns through.
 *
 * How a program's arguments reach it is the one thing the two targets disagree about: Linux hands the
 * kernel an argv, Windows hands CreateProcess one command line and the child splits it again. That
 * marshalling is part of making the call, so each side does its own and both take the same argv here.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Not a pipe. Minus one and not zero: zero is stdin on Linux and a valid, if null, handle on Windows. */
#define NYA_OS_PIPE_NONE ((NYA_OsPipe)(-1))

/** A wait that only ends when the process does. */
#define NYA_OS_PROCESS_WAIT_FOREVER ((u32)0xFFFFFFFF)

/**
 * Most processes and pipes one nya_os_process_wait_any call watches. Sixty four because
 * WaitForMultipleObjects takes no more than that many handles, and a process has two pipes.
 * */
#define NYA_OS_PROCESS_WAIT_MAX       64
#define NYA_OS_PROCESS_WAIT_PIPES_MAX (NYA_OS_PROCESS_WAIT_MAX * 2)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_OsProcessStatus  NYA_OsProcessStatus;
typedef struct NYA_OsProcess      NYA_OsProcess;
typedef struct NYA_OsProcessSpawn NYA_OsProcessSpawn;

/** One end of a pipe: a file descriptor on Linux, a HANDLE cast to this on Windows. */
typedef s64 NYA_OsPipe;

/** How a call here ended. One shape for every function, since none of them knows what a failure means. */
enum NYA_OsProcessStatus {
    /** It worked, and whatever it was asked for is in the out parameters. */
    NYA_OS_PROCESS_OK,
    /** Nothing to report yet: the process is still running, or the pipe had nothing ready to read. */
    NYA_OS_PROCESS_PENDING,
    /** The far end is gone: end of file on a pipe. */
    NYA_OS_PROCESS_CLOSED,
    /** The operating system refused. Which is for the caller above to make sense of. */
    NYA_OS_PROCESS_FAILED,
    NYA_OS_PROCESS_STATUS_COUNT,
};

/** A started process. Zeroed is none, which is what a spent one is left as. */
struct NYA_OsProcess {
    /** The process id on Linux, the process HANDLE cast to this on Windows. */
    u64 handle;

    /** Windows hands out a second handle for the child's main thread, which has to be closed too. */
    u64 thread;
};

/** What to start and how, for nya_os_process_spawn. */
struct NYA_OsProcessSpawn {
    /** What to run, looked up in PATH when it names no directory. */
    NYA_ConstCString program;

    /**
     * The whole argument vector the child is given, null terminated, `program` again in `arguments[0]`.
     * Separate from it because that is the shape execvp takes: what is run and what it is told it is
     * called are two different strings, even though every caller here passes the same one twice.
     * */
    const NYA_ConstCString* arguments;

    /** Where the child starts. Null or empty keeps this process's. */
    NYA_ConstCString working_directory;

    /**
     * `NAME=value` entries added to this process's own environment, null terminated. Null, or an empty
     * first entry, hands the child the environment unchanged. Not `char const*`: putenv takes the
     * string itself and keeps it, so it is the caller's to keep alive until the child has execed.
     * */
    NYA_CString* environment;

    /** Where the child's stdout and stderr go: the write end of a pipe each, or NYA_OS_PIPE_NONE. */
    NYA_OsPipe stdout_pipe;
    NYA_OsPipe stderr_pipe;

    /** With no pipes, whether the child writes to the null device instead of this process's own streams. */
    b8 suppress_output;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────────
 * PIPES
 * ─────────────────────────────────────────────────────────────
 */

/**
 * A pipe for a child's output: `out_write` is what it is given, `out_read` what this process keeps.
 *
 * Only the write end is inheritable, and the read end never blocks, so one process can hold several
 * children's pipes and read whichever has something without being stuck on one that has not written yet.
 * */
NYA_API NYA_OsProcessStatus nya_os_pipe_open(OUT NYA_OsPipe* out_read, OUT NYA_OsPipe* out_write) __attr_no_discard;

/** Closes one end. Closing NYA_OS_PIPE_NONE does nothing, so an unused end needs no branch. */
NYA_API void nya_os_pipe_close(NYA_OsPipe pipe);

/**
 * Reads whatever is in the pipe right now, up to `capacity` bytes, without ever waiting for more.
 *
 * PENDING when nothing is ready and the writer still holds its end, CLOSED at end of file. `out_taken`
 * is only meaningful on OK, and is never zero there.
 * */
NYA_API NYA_OsProcessStatus nya_os_pipe_read(NYA_OsPipe pipe, OUT u8* buffer, u64 capacity, OUT u64* out_taken) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────
 * PROCESSES
 * ─────────────────────────────────────────────────────────────
 */

/** This process's id. Unique among running processes, reused after one exits. */
NYA_API u32 nya_os_process_id(void) __attr_no_discard;

/**
 * Whether `name` names a program that could be run, and where it resolved to.
 *
 * The same lookup a spawn does without the fork: a name with a path separator in it is checked as it
 * stands, and a bare name is searched for on PATH exactly as execvp would search — so a true here is a
 * program nya_os_process_spawn will find, and a false is a missing dependency caught before the spawn
 * that would have failed obscurely with exit code 127. `out_path` receives the resolved path and may
 * be null when only the yes/no is wanted; `out_size` bounds it, NYA_OS_PATH_MAX being enough for any.
 *
 * False when it is on no PATH entry, when `name` is null or empty, or when the resolved path did not
 * fit `out_path`. This layer is below the assertion machinery, so a bad argument is refused, not asserted.
 * */
NYA_API b8 nya_os_process_which(const char* name, OUT char* out_path, u64 out_size) __attr_no_discard;

/**
 * Starts `spawn->program` and returns as soon as it is running, without waiting for it.
 *
 * FAILED when the operating system refused to start it, which on Linux includes a program that is not
 * on PATH: the fork succeeds and the child exits 127, so that one is reported by the exit code instead.
 * */
NYA_API NYA_OsProcessStatus nya_os_process_spawn(const NYA_OsProcessSpawn* spawn, OUT NYA_OsProcess* out_process) __attr_no_discard;

/**
 * Whether `process` has exited and with what code, waiting up to `timeout_ms` for it to.
 *
 * Zero asks and answers at once, NYA_OS_PROCESS_WAIT_FOREVER waits for the exit. PENDING while it is
 * still running. On OK the child has been reaped and the handle is spent: drop the caller's copy of it,
 * or a later call reaps an unrelated process that reused the id.
 *
 * The exit code is what the program returned, or 128 plus the signal that killed it, which is the shell's
 * convention and the only number Windows has to offer for both.
 * */
NYA_API NYA_OsProcessStatus nya_os_process_wait(NYA_OsProcess process, u32 timeout_ms, OUT s32* out_exit_code) __attr_no_discard;

/**
 * Sleeps until one of `processes` or `pipes` may have moved, or `timeout_ms` passes.
 *
 * A hint for when to call nya_os_process_wait again and nothing more: what woke it is not reported, and
 * it may return early for no reason at all. Both halves are passed because the two targets wake on
 * different things — Linux cannot wait on a process id, Windows cannot wait on an anonymous pipe — and a
 * caller that passes only one of them still waits correctly on one target and sleeps out the timeout on
 * the other. Anything past NYA_OS_PROCESS_WAIT_MAX is ignored rather than refused; this layer does not assert.
 * */
NYA_API void nya_os_process_wait_any(const NYA_OsProcess* processes, u32 process_count, const NYA_OsPipe* pipes, u32 pipe_count, u32 timeout_ms);

/**
 * Ends `process` now, without giving it a chance to clean up. SIGKILL on Linux, TerminateProcess on Windows.
 *
 * The process still has to be waited for afterwards: this only asks for the end, and the handle stays
 * spendable until nya_os_process_wait reaps it.
 * */
NYA_API NYA_OsProcessStatus nya_os_process_kill(NYA_OsProcess process) __attr_no_discard;
