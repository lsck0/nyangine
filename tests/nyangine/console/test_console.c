/**
 * The command registry the dev console and the command palette share: register, run a typed line,
 * fuzzy-search for a palette, and run a searched result by index. Base only, so no app or SDL is brought
 * up — the registry is a table and a dispatcher and nothing more.
 **/

#include <stdio.h>
#include <string.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

static char CAPTURE[4096] = { 0 };
static u32  CAPTURE_LEN   = 0;

/** An output sink that appends each line, so a test can read what a command answered. */
static void capture(void* user, NYA_ConstCString line) {
  (void)user;
  CAPTURE_LEN += (u32)snprintf(CAPTURE + CAPTURE_LEN, sizeof(CAPTURE) - CAPTURE_LEN, "%s\n", line);
}

static u32 SPAWN_CALLS = 0;

static void cmd_spawn(NYA_ConsoleInvocation* call) {
  SPAWN_CALLS++;
  if (call->argument_count < 1) {
    nya_console_printf(call, "usage: spawn <kind>");
    return;
  }
  nya_console_printf(call, "spawned %s x%u", call->arguments[0], call->argument_count);
}

static void cmd_quit(NYA_ConsoleInvocation* call) { nya_console_printf(call, "bye"); }

int main(void) {
  nya_console_reset();

  nya_assert(nya_console_register("spawn", "spawn <kind> — add an entity", cmd_spawn).ok);
  nya_assert(nya_console_register("spawn_wave", "spawn a wave of enemies", cmd_spawn).ok);
  nya_assert(nya_console_register("quit", "quit the game", cmd_quit).ok);
  nya_assert(nya_console_count() == 3);

  // a typed line dispatches, splits arguments, and answers through the sink.
  CAPTURE_LEN = 0;
  nya_assert(nya_console_run("spawn goblin", capture, nullptr).ok);
  nya_assert(SPAWN_CALLS == 1);
  nya_assert(strstr(CAPTURE, "spawned goblin x1") != nullptr);

  // a double-quoted argument stays one token.
  CAPTURE_LEN = 0;
  nya_assert(nya_console_run("spawn \"big goblin\"", capture, nullptr).ok);
  nya_assert(strstr(CAPTURE, "spawned big goblin x1") != nullptr);

  // an unknown command reports itself and returns NOT_FOUND.
  CAPTURE_LEN     = 0;
  NYA_Error error = nya_console_run("nope", capture, nullptr);
  nya_assert(!error.ok && error.kind == NYA_ERROR_NOT_FOUND);
  nya_assert(strstr(CAPTURE, "unknown command: nope") != nullptr);

  // fuzzy search, a palette's filter: a scattered subsequence finds spawn.
  NYA_ConsoleMatch matches[8] = { 0 };
  u32              found       = nya_console_search("spwn", matches, 8);
  nya_assert(found >= 1);
  nya_assert(strncmp(nya_console_name_at(matches[0].index), "spawn", 5) == 0);

  // a word in a description matches too.
  found = nya_console_search("wave", matches, 8);
  nya_assert(found >= 1);

  // an empty query returns every command, in registration order.
  found = nya_console_search("", matches, 8);
  nya_assert(found == 3);
  nya_assert(strcmp(nya_console_name_at(matches[0].index), "spawn") == 0);
  nya_assert(strstr(nya_console_description_at(matches[0].index), "add an entity") != nullptr);

  // run a searched result by index, the way a palette runs the selected row.
  found = nya_console_search("quit", matches, 8);
  nya_assert(found >= 1);
  CAPTURE_LEN = 0;
  nya_assert(nya_console_run_at(matches[0].index, nullptr, 0, capture, nullptr).ok);
  nya_assert(strstr(CAPTURE, "bye") != nullptr);

  // unregister drops it and keeps the rest.
  nya_console_unregister("quit");
  nya_assert(nya_console_count() == 2);
  nya_assert(nya_console_search("quit", matches, 8) == 0);

  nya_log_info("test_console: ok.");
  return 0;
}
