/**
 * Starting a process, reading its pipes, waiting for it and killing it.
 *
 * The primitives only: what base_command.h builds on top of them, capture and all, is covered by
 * tests/nyangine/base/test_command.c.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Longest output one of these children writes, with room to notice if it wrote more. */
#define OUTPUT_MAX 256

/** What a killed child reports: 128 plus SIGKILL, which is what the Windows side terminates with too. */
#define KILLED_EXIT_CODE 137

/** Reads `pipe` to end of file into `out`, which is left null terminated. */
static u64 drain(NYA_OsPipe pipe, u8* out, u64 capacity) {
    u64 length = 0;

    while (true) {
        u64                 taken  = 0;
        NYA_OsProcessStatus status = nya_os_pipe_read(pipe, out + length, capacity - length - 1, &taken);

        if (status == NYA_OS_PROCESS_OK) {
            length += taken;
            continue;
        }
        if (status == NYA_OS_PROCESS_PENDING) {
            nya_os_process_wait_any(nullptr, 0, &pipe, 1, NYA_OS_PROCESS_WAIT_FOREVER);
            continue;
        }

        break;
    }

    out[length] = '\0';
    return length;
}

s32 main(void) {
    // ── This process has an id, and it is the same one every time.
    {
        u32 id = nya_os_process_id();
        nya_check(id > 0, "this process should have an id");
        nya_check(id == nya_os_process_id(), "the id should not change between calls");
    }

    // ── A child writes to the pipes it was given, and the exit code is its own.
    {
        NYA_OsPipe out_read    = NYA_OS_PIPE_NONE;
        NYA_OsPipe out_write   = NYA_OS_PIPE_NONE;
        NYA_OsPipe error_read  = NYA_OS_PIPE_NONE;
        NYA_OsPipe error_write = NYA_OS_PIPE_NONE;

        nya_check(nya_os_pipe_open(&out_read, &out_write) == NYA_OS_PROCESS_OK, "opening a pipe should succeed");
        nya_check(nya_os_pipe_open(&error_read, &error_write) == NYA_OS_PROCESS_OK, "opening a second pipe should succeed");

        NYA_OsProcessSpawn spawn = {
            .program     = "sh",
            .arguments   = (NYA_ConstCString[]){ "sh", "-c", "printf out; printf err 1>&2; exit 3", nullptr },
            .stdout_pipe = out_write,
            .stderr_pipe = error_write,
        };

        NYA_OsProcess child = { 0 };
        nya_check(nya_os_process_spawn(&spawn, &child) == NYA_OS_PROCESS_OK, "spawning sh should succeed");

        // the parent's copies, or the read ends would never reach end of file.
        nya_os_pipe_close(out_write);
        nya_os_pipe_close(error_write);

        u8 out[OUTPUT_MAX]   = { 0 };
        u8 error[OUTPUT_MAX] = { 0 };
        nya_check(drain(out_read, out, sizeof(out)) == 3 && strcmp((char*)out, "out") == 0, "stdout should arrive whole, got '%s'", (char*)out);
        nya_check(
            drain(error_read, error, sizeof(error)) == 3 && strcmp((char*)error, "err") == 0,
            "stderr should arrive whole, got '%s'",
            (char*)error
        );

        nya_os_pipe_close(out_read);
        nya_os_pipe_close(error_read);

        s32 exit_code = 0;
        nya_check(nya_os_process_wait(child, NYA_OS_PROCESS_WAIT_FOREVER, &exit_code) == NYA_OS_PROCESS_OK, "waiting for the child should succeed");
        nya_check(exit_code == 3, "the child's own exit code should come back, got %d", exit_code);
    }

    // ── A wait that runs out says so, and a killed child is still waited for.
    {
        NYA_OsProcessSpawn spawn = {
            .program         = "sh",
            .arguments       = (NYA_ConstCString[]){ "sh", "-c", "sleep 30", nullptr },
            .stdout_pipe     = NYA_OS_PIPE_NONE,
            .stderr_pipe     = NYA_OS_PIPE_NONE,
            .suppress_output = true,
        };

        NYA_OsProcess child = { 0 };
        nya_check(nya_os_process_spawn(&spawn, &child) == NYA_OS_PROCESS_OK, "spawning a long sleep should succeed");

        s32 exit_code = 0;
        nya_check(nya_os_process_wait(child, 0, &exit_code) == NYA_OS_PROCESS_PENDING, "asking without waiting should find it running");

        u64 started_ms = nya_os_time_monotonic_ns() / 1'000'000ULL;
        nya_check(nya_os_process_wait(child, 20, &exit_code) == NYA_OS_PROCESS_PENDING, "a 20 ms wait should run out on a 30 second sleep");
        nya_check(nya_os_time_monotonic_ns() / 1'000'000ULL - started_ms < 1000, "and should have waited nothing like the sleep itself");

        nya_check(nya_os_process_kill(child) == NYA_OS_PROCESS_OK, "killing it should succeed");
        nya_check(nya_os_process_wait(child, NYA_OS_PROCESS_WAIT_FOREVER, &exit_code) == NYA_OS_PROCESS_OK, "a killed child still has to be waited for");
        nya_check(exit_code == KILLED_EXIT_CODE, "a killed child should report %d, got %d", KILLED_EXIT_CODE, exit_code);
    }

    // ── A child starts where it was told to, with the variables it was given.
    {
        NYA_OsPipe out_read  = NYA_OS_PIPE_NONE;
        NYA_OsPipe out_write = NYA_OS_PIPE_NONE;
        nya_check(nya_os_pipe_open(&out_read, &out_write) == NYA_OS_PROCESS_OK, "opening a pipe should succeed");

        char variable[] = "NYA_TEST_OS_PROCESS=set";

        NYA_OsProcessSpawn spawn = {
            .program           = "sh",
            .arguments         = (NYA_ConstCString[]){ "sh", "-c", "printf %s \"$NYA_TEST_OS_PROCESS\"; basename \"$PWD\"", nullptr },
            .working_directory = "src",
            .environment       = (NYA_CString[]){ variable, nullptr },
            .stdout_pipe       = out_write,
            .stderr_pipe       = NYA_OS_PIPE_NONE,
        };

        NYA_OsProcess child = { 0 };
        nya_check(nya_os_process_spawn(&spawn, &child) == NYA_OS_PROCESS_OK, "spawning sh should succeed");
        nya_os_pipe_close(out_write);

        u8 out[OUTPUT_MAX] = { 0 };
        (void)drain(out_read, out, sizeof(out));
        nya_os_pipe_close(out_read);

        s32 exit_code = 0;
        nya_check(nya_os_process_wait(child, NYA_OS_PROCESS_WAIT_FOREVER, &exit_code) == NYA_OS_PROCESS_OK, "waiting for the child should succeed");
        nya_check(strcmp((char*)out, "setsrc\n") == 0, "the child should have had the variable and the directory, got '%s'", (char*)out);

        // the variable was the child's alone.
        nya_check(getenv("NYA_TEST_OS_PROCESS") == nullptr, "the parent's environment should be untouched");
    }

    // ── Nothing to work with is refused rather than asserted, since this layer is below assertions.
    {
        NYA_OsProcess nothing   = { 0 };
        s32           exit_code = 0;

        nya_check(nya_os_process_wait(nothing, 0, &exit_code) == NYA_OS_PROCESS_FAILED, "waiting for no process should fail");
        nya_check(nya_os_process_kill(nothing) == NYA_OS_PROCESS_FAILED, "killing no process should fail");

        u8  byte  = 0;
        u64 taken = 0;
        nya_check(nya_os_pipe_read(NYA_OS_PIPE_NONE, &byte, 1, &taken) == NYA_OS_PROCESS_FAILED, "reading no pipe should fail");

        // closing what was never opened is the one that is allowed, so an unused end needs no branch.
        nya_os_pipe_close(NYA_OS_PIPE_NONE);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
