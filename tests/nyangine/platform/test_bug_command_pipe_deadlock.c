/**
 * Regression test for nya_command_wait deadlocking on a child that fills the stderr pipe.
 *
 * The capture path drained the two pipes in sequence: stdout to EOF, then stderr. A pipe holds
 * about sixty four kibibytes, so a child writing more than that to stderr blocks on the write. It
 * therefore never exits, never closes its stdout end, and the parent's stdout read never sees EOF.
 * Both sides wait on the other forever.
 *
 * Reachable from the build system itself, which is the main user of this API and captures output
 * from clang — a compile with a few hundred diagnostics clears sixty four kibibytes of stderr
 * easily, and that is exactly the run where hanging is least welcome.
 *
 * **Watchdog, not an assertion.** On the unfixed code this hangs rather than failing, so a plain
 * test would stall the suite instead of reporting. The watchdog turns it into a clean non-zero exit.
 **/
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Well past a pipe buffer on any platform this builds for. */
#define STDERR_BYTES 1048576

#define WATCHDOG_TIMEOUT_MS 20000

static atomic b8 finished = false;

static int watchdog(void* data) {
  nya_unused(data);

  u64 waited = 0;
  while (waited < WATCHDOG_TIMEOUT_MS) {
    if (atomic_load(&finished)) return 0;
    SDL_Delay(25);
    waited += 25;
  }

  fprintf(
    stderr,
    "\nDEADLOCK: nya_command_wait made no progress in %d ms.\n"
    "The child is blocked writing stderr while the parent drains stdout to EOF first.\n",
    WATCHDOG_TIMEOUT_MS
  );
  fflush(stderr);
  _exit(1);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_bug_command_pipe_deadlock");

  SDL_Thread* guard = SDL_CreateThread(watchdog, "watchdog", nullptr);
  nya_assert(guard != nullptr, "SDL_CreateThread failed for the watchdog: %s", SDL_GetError());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a child that floods stderr while stdout stays open
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: %d bytes of stderr\n", STDERR_BYTES);
  {
    // `yes` piped through head is portable and needs no temporary file. The stdout write keeps the
    // parent's first read waiting, which is what makes the ordering matter.
    NYA_String* script = nya_string_sprintf(
      arena,
      "printf 'on stdout\\n'; yes 'flooding stderr with a reasonably long line of text' | head -c %d 1>&2; printf 'done\\n'",
      STDERR_BYTES
    );

    NYA_Command command = {
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "/bin/sh",
      .arguments = { "-c", nya_string_to_cstring(arena, script) },
      .arena     = arena,
    };

    NYA_EXPECT(nya_command_run(&command));

    nya_assert(command.exit_code == 0, "the child exited with %d", command.exit_code);
    nya_assert(
      command.stderr_content->length == STDERR_BYTES,
      "captured " FMTu64 " bytes of stderr, expected %d",
      command.stderr_content->length,
      STDERR_BYTES
    );
    nya_assert(nya_string_contains(command.stdout_content, "on stdout"), "the stdout capture lost its first line");
    nya_assert(nya_string_contains(command.stdout_content, "done"), "the child did not run to completion");

    nya_command_destroy(&command);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the mirror case, a flood on stdout
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: %d bytes of stdout\n", STDERR_BYTES);
  {
    NYA_String* script = nya_string_sprintf(
      arena,
      "printf 'on stderr\\n' 1>&2; yes 'flooding stdout with a reasonably long line of text' | head -c %d; printf 'done\\n' 1>&2",
      STDERR_BYTES
    );

    NYA_Command command = {
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "/bin/sh",
      .arguments = { "-c", nya_string_to_cstring(arena, script) },
      .arena     = arena,
    };

    NYA_EXPECT(nya_command_run(&command));

    nya_assert(command.exit_code == 0, "the child exited with %d", command.exit_code);
    nya_assert(
      command.stdout_content->length == STDERR_BYTES,
      "captured " FMTu64 " bytes of stdout, expected %d",
      command.stdout_content->length,
      STDERR_BYTES
    );
    nya_assert(nya_string_contains(command.stderr_content, "done"), "the child did not run to completion");

    nya_command_destroy(&command);
  }
  printf("  PASSED\n");

  atomic_store(&finished, true);
  SDL_WaitThread(guard, nullptr);

  nya_arena_destroy(arena);
  SDL_Quit();

  printf("PASSED: test_bug_command_pipe_deadlock\n");
  return 0;
}
