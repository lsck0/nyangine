/**
 * Chat end to end: one server, two clients, three processes, real UDP.
 *
 * ## Why this test forks
 *
 * A client is a process singleton — one _NYA_NET_CLIENT, one connection, one chat history. So "two
 * players talking" cannot be written in one process without testing something other than what ships:
 * the two clients would share the history they are each supposed to receive independently, which is
 * exactly the thing worth checking.
 *
 * So the parent is the server and each client is a forked child with its own copy of everything. The
 * fork happens **before** the server binds, so no child inherits the listening socket — a duplicated
 * UDP descriptor refers to the same open file description, and a child reading from it would steal
 * datagrams meant for the server. The children learn the port through a pipe instead, which also
 * removes the race where a client connects before there is anything listening.
 *
 * What it defends:
 *
 * - **A line reaches every client, including the one that sent it.** The sender sees its own words
 *   only when the server says so, which is what makes what you see what everyone else saw.
 * - **A client cannot choose its own name.** A message carrying `"name": "alice"` from bob's socket
 *   arrives everywhere attributed to bob.
 * - **Hostile text is neutralised before it is stored.** Control characters, a bidirectional override
 *   and an invalid byte go in; a clean, well formed line comes out — on every receiver.
 * - **Flooding is bounded.** Twenty lines sent as fast as the socket takes them arrive as at most
 *   NYA_NET_CHAT_BURST.
 * - **System lines are attributed to nobody**, and are not rate limited.
 *
 * The phases are driven by the server broadcasting a system line rather than by sleeping, so the two
 * children stay in step without either guessing how long the other needs.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FLAG_REPLICATED (1ULL << 2)

#define FIRST_PORT      47960
#define PUMP_TIMEOUT_MS 15000
#define TICK_SECONDS    (1.0F / 60.0F)

/** How many lines bob throws at the server in the flood phase. Far above NYA_NET_CHAT_BURST. */
#define FLOOD_ATTEMPTS 20

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/*
 * ─────────────────────────────────────────────────────────
 * WHAT BOTH SIDES NEED TO BE A GAME AT ALL
 * ─────────────────────────────────────────────────────────
 */

static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  nya_unused(entity, command, delta_time_s);
}

static void sample_command(OUT NYA_NetCommand* command) {
  command->actions = 0;
}

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(peer, name);

  return nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CHAT HOOKS, WHICH ARE THE WHOLE INTEGRATION
 * ─────────────────────────────────────────────────────────
 */

/** How many chat events the server has been handed. Counts attempts, including ones the limit refused. */
static u32 SERVER_CHAT_SEEN = 0;

static void on_client_event(NYA_NetPeerId peer, const NYA_Object* event) {
  if (nya_net_chat_server_consume(peer, event)) {
    SERVER_CHAT_SEEN++;
    return;
  }

  // Where the game's own events would go.
}

static void on_game_event(const NYA_Object* event) {
  if (nya_net_chat_client_consume(event)) return;

  // Where the game's own events would go.
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CLIENT SIDE, RUN IN A CHILD
 * ─────────────────────────────────────────────────────────
 */

static u64 CHILD_TICK = 1;

/** One tick of the client and nothing else — a child has no server to run. */
static void client_pump(void) {
  nya_net_client_tick(CHILD_TICK, TICK_SECONDS);
  nya_system_sim_apply_commands();

  CHILD_TICK++;
}

/** Ticks for `milliseconds`, so lines that are in flight have somewhere to arrive. */
static void client_pump_for(u32 milliseconds) {
  u64 deadline = nya_clock_get_monotonic_ms() + milliseconds;

  while (nya_clock_get_monotonic_ms() < deadline) {
    client_pump();
    sleep_ms(2);
  }
}

/** The first line whose text starts with `prefix`, or null. How a phase finds its own messages. */
static const NYA_NetChatMessage* find_line(NYA_ConstCString prefix) {
  u64 length = strlen(prefix);

  for (u32 i = 0; i < nya_net_chat_count(); i++) {
    const NYA_NetChatMessage* message = nya_net_chat_at(i);

    if (strncmp(message->text, prefix, length) == 0) return message;
  }

  return nullptr;
}

static u32 count_lines(NYA_ConstCString prefix) {
  u64 length = strlen(prefix);
  u32 found  = 0;

  for (u32 i = 0; i < nya_net_chat_count(); i++) {
    if (strncmp(nya_net_chat_at(i)->text, prefix, length) == 0) found++;
  }

  return found;
}

/** Pumps until a system line saying exactly `text` arrives. The phase gate. */
static b8 wait_for_system(NYA_ConstCString text) {
  u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while (nya_clock_get_monotonic_ms() < deadline) {
    for (u32 i = 0; i < nya_net_chat_count(); i++) {
      const NYA_NetChatMessage* message = nya_net_chat_at(i);

      if (message->is_system && nya_string_equals(message->text, text)) return true;
    }

    client_pump();
    sleep_ms(2);
  }

  return false;
}

/**
 * What alice says: a line built to be nasty rather than to be read.
 *
 * Leading and trailing spaces, an interior run of them, two C0 controls, a right-to-left override and
 * a byte that is not UTF-8 at all. Written as escapes so it survives an editor that would helpfully
 * normalise it.
 * */
#define MESSY_INPUT  "  hi\x01\x02  there  \xE2\x80\xAE\xFF  "

/** What every receiver must end up with: trimmed, collapsed, stripped, and the bad byte as U+FFFD. */
#define MESSY_EXPECT "hi there \xEF\xBF\xBD"

#define SPOOF_TEXT "i am totally alice"

/** Bob's impersonation attempt: a chat event with a name and a peer id it has no right to. */
static void send_spoofed_line(void) {
  NYA_Arena* scratch = nya_arena_create(.name = "test_chat_spoof");
  defer      nya_arena_destroy(scratch);

  NYA_Object* event = nya_object_create(scratch);

  nya_object_set(event, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "chat" });
  nya_object_set(event, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)SPOOF_TEXT });

  // The fields the server is supposed to ignore entirely.
  nya_object_set(event, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "alice" });
  nya_object_set(event, "sender", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 999 });
  nya_object_set(event, "system", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

  NYA_EXPECT(nya_net_client_send_event(event));
}

/**
 * Whether sanitising `input` produced nothing but U+FFFD.
 *
 * Written as "every three bytes are EF BF BD" rather than as an expected string, because how many
 * replacements a malformed run collapses to is nya_utf8_next's business — what this asserts is that
 * none of the original bytes came through, which is the property that matters.
 * */
static b8 all_replacement(NYA_ConstCString input) {
  char out[NYA_NET_CHAT_TEXT_MAX] = { 0 };

  u64 written = nya_net_chat_sanitize(input, out, sizeof(out));

  if (written == 0 || written % 3 != 0) return false;

  for (u64 i = 0; i < written; i += 3) {
    if ((u8)out[i] != 0xEF || (u8)out[i + 1] != 0xBF || (u8)out[i + 2] != 0xBD) return false;
  }

  return true;
}

/** A whole client, start to finish. Returns the process's exit status. */
static s32 child_main(u32 index, s32 port_pipe) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_ConstCString name    = index == 0 ? "alice" : "bob";
  b8               is_bob  = index == 1;

  u16 port = 0;

  // Blocks until the parent has actually bound, which is what removes the connect-before-listen race.
  if (read(port_pipe, &port, sizeof(port)) != (ssize_t)sizeof(port)) {
    printf("  [%s] never learned the port\n", name);
    return 1;
  }

  (void)close(port_pipe);

  NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, name, (NYA_NetClientConfig){
    .replicated_flag   = FLAG_REPLICATED,
    .on_apply_command  = nya_callback(apply_movement),
    .on_sample_command = nya_callback(sample_command),
    .on_game_event     = nya_callback(on_game_event),
  }));

  u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while (nya_net_client_state() != NYA_NET_CLIENT_PLAYING && nya_clock_get_monotonic_ms() < deadline) {
    client_pump();
    sleep_ms(2);
  }

  nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "[%s] the handshake did not complete", name);

  printf("  [%s] playing on port %u\n", name, port);

  // ── phase one: everyone says one thing ────────────────────────────────────
  nya_assert(wait_for_system("go"), "[%s] never received the go system line", name);

  /*
   * The system line itself, checked here rather than in a phase of its own.
   *
   * It is the one message in this test nobody sent, so its sender must be unset — a system line that
   * arrived attributed to a peer would be indistinguishable from that peer saying it.
   */
  {
    const NYA_NetChatMessage* system_line = find_line("go");

    nya_assert(system_line != nullptr, "[%s] lost the go line", name);
    nya_assert(system_line->is_system, "[%s] the go line is not flagged as a system line", name);
    nya_assert(!nya_net_peer_is_set(system_line->sender), "[%s] a system line was attributed to a peer", name);
  }

  if (is_bob) {
    send_spoofed_line();
  } else {
    NYA_EXPECT(nya_net_chat_send(MESSY_INPUT));
  }

  // Both lines have to land on both clients, so this waits for the second one rather than its own.
  deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while ((find_line(MESSY_EXPECT) == nullptr || find_line(SPOOF_TEXT) == nullptr) &&
         nya_clock_get_monotonic_ms() < deadline) {
    client_pump();
    sleep_ms(2);
  }

  // ── what alice said, as everyone received it ──────────────────────────────
  {
    const NYA_NetChatMessage* line = find_line(MESSY_EXPECT);

    nya_assert(line != nullptr, "[%s] alice's line never arrived, or arrived unsanitised", name);

    // Exact, not a prefix: a trailing space or a surviving control character would still match a
    // prefix test, and both are the bug.
    nya_assert(nya_string_equals(line->text, MESSY_EXPECT), "[%s] alice's line is \"%s\"", name, line->text);
    nya_assert(nya_string_equals(line->name, "alice"), "[%s] alice's line is attributed to \"%s\"", name, line->name);
    nya_assert(!line->is_system, "[%s] a player line was flagged as a system line", name);
    nya_assert(nya_net_peer_is_set(line->sender), "[%s] a player line arrived with no sender", name);
  }

  // ── what bob said, and the name he asked for and did not get ──────────────
  {
    const NYA_NetChatMessage* line = find_line(SPOOF_TEXT);

    nya_assert(line != nullptr, "[%s] bob's line never arrived", name);
    nya_assert(nya_string_equals(line->name, "bob"), "[%s] bob impersonated \"%s\"", name, line->name);
    nya_assert(line->sender.index != 999, "[%s] bob chose his own peer id", name);

    /*
     * And he does not get to be the server either.
     *
     * The spoofed event set "system": true, which the relay never copies — a client that could set it
     * would be able to put words in the server's mouth, which is worse than impersonating a player.
     */
    nya_assert(!line->is_system, "[%s] a client made itself the server", name);
  }

  printf("  [%s] phase one passed\n", name);

  // ── phase two: bob floods ─────────────────────────────────────────────────
  nya_assert(wait_for_system("flood"), "[%s] never received the flood system line", name);

  if (is_bob) {
    for (u32 i = 0; i < FLOOD_ATTEMPTS; i++) {
      char line[64] = { 0 };
      (void)snprintf(line, sizeof(line), "flood %u", i);

      // Not NYA_EXPECT: the send itself succeeds every time. The limit is the *server's* and refusing
      // a line is not an error the sender is told about, which is the point of checking the receipts.
      (void)nya_net_chat_send(line);
    }
  }

  /*
   * Long enough for twenty reliable messages to cross a loopback socket several times over, and short
   * enough that the bucket refills by at most one token — NYA_NET_CHAT_REFILL_MS is 1500.
   */
  client_pump_for(1200);

  u32 arrived = count_lines("flood ");

  printf("  [%s] %u of %u flooded lines arrived\n", name, arrived, FLOOD_ATTEMPTS);

  nya_assert(arrived >= 1, "[%s] the rate limit swallowed every line, including the first", name);
  nya_assert(arrived <= NYA_NET_CHAT_BURST, "[%s] %u lines got through a burst of %d", name, arrived, NYA_NET_CHAT_BURST);

  nya_net_client_disconnect();

  printf("  [%s] done\n", name);

  return 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE SERVER SIDE, RUN IN THE PARENT
 * ─────────────────────────────────────────────────────────
 */

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the sanitiser's edges, which a wire test cannot reach precisely
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: sanitising\n");
  {
    char out[NYA_NET_CHAT_TEXT_MAX] = { 0 };

    // ── whitespace is collapsed and trimmed ──────────────────────────────────
    nya_assert(nya_net_chat_sanitize("   a   b   ", out, sizeof(out)) == 3);
    nya_assert(nya_string_equals(out, "a b"), "collapsed to \"%s\"", out);

    // ── a line with nothing left is a line with nothing in it ────────────────
    nya_assert(nya_net_chat_sanitize("\x01\x02\x1F\x7F   ", out, sizeof(out)) == 0);
    nya_assert(out[0] == '\0', "an emptied line is still terminated");

    // Null in, empty out, rather than a crash: callers pass whatever a text field gave them.
    nya_assert(nya_net_chat_sanitize(nullptr, out, sizeof(out)) == 0);

    /*
     * ── truncation lands on a codepoint boundary ───────────────────────────
     *
     * Three bytes each, into eight bytes of capacity. Two fit with the terminator and the third does
     * not, so the answer must be exactly six — a byte-counting truncation would write seven and leave
     * a third of a character at the end, which is the malformed input this function exists to remove.
     */
    char small[8] = { 0 };

    nya_assert(nya_net_chat_sanitize("あああああ", small, sizeof(small)) == 6, "truncated mid-sequence");
    nya_assert(nya_utf8_count(small) == 2, "truncation split a codepoint");

    // A capacity that cannot hold even one character still produces a valid empty string.
    char tiny[1] = { 0 };
    nya_assert(nya_net_chat_sanitize("あ", tiny, sizeof(tiny)) == 0);
    nya_assert(tiny[0] == '\0');

    // ── malformed input becomes replacement characters, never itself ─────────
    // An overlong '/', the classic filter bypass: a check for a literal '/' never sees this spelling.
    nya_assert(all_replacement("\xC0\xAF"), "an overlong encoding survived");

    // A surrogate half, which is not encodable in UTF-8 at all.
    nya_assert(all_replacement("\xED\xA0\x80"), "a surrogate survived");

    // A truncated three-byte lead with nothing following it.
    nya_assert(all_replacement("\xE2\x80"), "a truncated sequence survived");

    // ── the history ring keeps the newest and drops the oldest ───────────────
    nya_net_chat_clear();

    nya_assert(nya_net_chat_count() == 0);
    nya_assert(nya_net_chat_at(0) == nullptr, "reading past the end returns null");

    for (u32 i = 0; i < NYA_NET_CHAT_HISTORY + 10; i++) {
      char line[32] = { 0 };
      (void)snprintf(line, sizeof(line), "line %u", i);

      nya_net_chat_append_local(line);
    }

    nya_assert(nya_net_chat_count() == NYA_NET_CHAT_HISTORY, "the ring grew past its bound");
    nya_assert(nya_string_equals(nya_net_chat_at(0)->text, "line 10"), "the oldest kept line is \"%s\"",
               nya_net_chat_at(0)->text);
    nya_assert(nya_string_equals(nya_net_chat_at(NYA_NET_CHAT_HISTORY - 1)->text, "line 73"), "the newest line is wrong");
    nya_assert(nya_net_chat_at(NYA_NET_CHAT_HISTORY) == nullptr);

    // Cleared before the fork: a child inherits this history, and lines from here would show up in
    // the assertions below as if they had come off the wire.
    nya_net_chat_clear();

    nya_assert(nya_net_chat_count() == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: two clients and a server
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: two clients and a server exchange chat over UDP\n");

  /*
   * Forked before the server binds anything.
   *
   * See the note at the top: a child that inherited the listening socket would compete with the
   * server for datagrams on it. Forking first means there is nothing to inherit.
   */
  s32 pipes[2][2] = { 0 };
  pid_t children[2] = { 0 };

  for (u32 i = 0; i < 2; i++) {
    nya_assert(pipe(pipes[i]) == 0, "could not create a pipe for child %u", i);
  }

  for (u32 i = 0; i < 2; i++) {
    pid_t pid = fork();

    nya_assert(pid >= 0, "could not fork child %u", i);

    if (pid == 0) {
      // The child needs its own read end and nothing else. The other child's pipe and every write end
      // are closed so a parent that dies cannot leave a child blocked on a read forever.
      (void)close(pipes[i][1]);
      (void)close(pipes[1 - i][0]);
      (void)close(pipes[1 - i][1]);

      s32 status = child_main(i, pipes[i][0]);

      /*
       * _exit rather than exit or a return.
       *
       * A child holds a copy of everything the parent had allocated before the fork and did not
       * allocate any of it, so running the parent's atexit handlers here would free it twice in the
       * accounting and report leaks that belong to a process that is still using them.
       */
      _exit(status);
    }

    children[i] = pid;

    (void)close(pipes[i][0]);
  }

  NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
    .replicated_flag   = FLAG_REPLICATED,
    .on_apply_command  = nya_callback(apply_movement),
    .on_spawn_player   = nya_callback(spawn_player),
    .on_client_event   = nya_callback(on_client_event),
  }));

  u16 port = 0;
  for (u16 candidate = FIRST_PORT; candidate < FIRST_PORT + 16; candidate++) {
    if (nya_net_server_listen(candidate).ok) {
      port = candidate;
      break;
    }
  }

  nya_assert(port != 0, "could not bind any port in the test range");

  printf("  [server] listening on %u\n", port);

  for (u32 i = 0; i < 2; i++) {
    nya_assert(write(pipes[i][1], &port, sizeof(port)) == (ssize_t)sizeof(port), "could not tell child %u the port", i);
    (void)close(pipes[i][1]);
  }

  u64 tick = 1;

  // ── wait for both players ────────────────────────────────────────────────
  u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while (nya_net_server_peer_count() < 2 && nya_clock_get_monotonic_ms() < deadline) {
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    tick++;

    sleep_ms(2);
  }

  nya_assert(nya_net_server_peer_count() == 2, "only %u of 2 clients joined", nya_net_server_peer_count());

  printf("  [server] both players joined\n");

  // ── phase one ────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_net_chat_broadcast_system("go"));

  deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while (SERVER_CHAT_SEEN < 2 && nya_clock_get_monotonic_ms() < deadline) {
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    tick++;

    sleep_ms(2);
  }

  nya_assert(SERVER_CHAT_SEEN >= 2, "the server saw %u chat events, expected both players to speak", SERVER_CHAT_SEEN);

  /*
   * A beat before the next phase, so both relays are on the wire before the flood starts.
   *
   * Deliberately short: every millisecond here refills bob's bucket, and the flood assertion is
   * tightest when it does not.
   */
  for (u32 i = 0; i < 30; i++) {
    nya_net_server_tick(tick, TICK_SECONDS);
    tick++;
    sleep_ms(2);
  }

  // ── phase two ────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_net_chat_broadcast_system("flood"));

  u32 before_flood = SERVER_CHAT_SEEN;

  deadline = nya_clock_get_monotonic_ms() + 4000;

  while (nya_clock_get_monotonic_ms() < deadline) {
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    tick++;

    sleep_ms(2);
  }

  /*
   * The server was *handed* every flooded line — the limit is not a transport that dropped them.
   *
   * Worth asserting separately: if the packets had never arrived, the receipts on the client side
   * would look identical to a working rate limit, and the test would pass for the wrong reason.
   */
  u32 delivered = SERVER_CHAT_SEEN - before_flood;

  nya_assert(delivered >= FLOOD_ATTEMPTS, "the server was handed %u of %d flooded lines; the limit was not what stopped them",
             delivered, FLOOD_ATTEMPTS);

  printf("  [server] received all %u flooded lines and relayed at most %d\n", delivered, NYA_NET_CHAT_BURST);

  // ── collect the children ─────────────────────────────────────────────────
  for (u32 i = 0; i < 2; i++) {
    s32 status = 0;

    // Kept ticking while waiting: a child still has to receive the disconnect it is waiting on.
    pid_t done = 0;

    deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

    while (done == 0 && nya_clock_get_monotonic_ms() < deadline) {
      done = waitpid(children[i], &status, WNOHANG);

      if (done == 0) {
        nya_net_server_tick(tick, TICK_SECONDS);
        tick++;
        sleep_ms(2);
      }
    }

    nya_assert(done == children[i], "child %u never finished", i);
    nya_assert(WIFEXITED(status), "child %u crashed rather than exiting", i);
    nya_assert(WEXITSTATUS(status) == 0, "child %u failed with status %d", i, WEXITSTATUS(status));
  }

  printf("  [server] both clients passed\n");

  nya_net_server_stop();

  nya_world_destroy(world);
  nya_system_callback_deinit();

  printf("PASSED\n");

  return 0;
}
