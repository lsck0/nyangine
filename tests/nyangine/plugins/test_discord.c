/**
 * The Discord presence payload, and the frame it travels in.
 *
 * There is no Discord client in CI, so what can be tested is everything up to the socket: the JSON
 * that would be sent, the framing around it, and the change detection that decides whether to send
 * at all. That is where the bugs are — a malformed frame is answered by the client closing the
 * connection with no diagnostic, so "it produced valid JSON" is the whole safety net.
 *
 * Two behaviours in particular are load bearing.
 *
 * **Escaping.** A player's party name reaches this code. An unescaped quote in it produces a frame
 * that ends the connection, and a control character produces JSON no parser accepts.
 *
 * **Change detection by content, not by pointer.** A caller formatting its details line into a
 * stack buffer hands over a different pointer every frame with identical bytes behind it. Comparing
 * pointers would report a change every frame and write to the socket every frame, which is exactly
 * what the rate limiter exists to prevent.
 *
 * Reaches the module's internals directly. That is what a unity build is for, and the alternative —
 * exposing a payload builder publicly so a test can call it — would widen the API for the test's
 * sake.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Whether `haystack` contains `needle`, on plain C strings. */
static b8 contains(NYA_ConstCString haystack, NYA_ConstCString needle) {
  return strstr(haystack, needle) != nullptr;
}

/** Parses `text` as JSON, asserting it is well formed. That it parses at all is most of the point. */
static NYA_Object* parse(NYA_Arena* arena, NYA_ConstCString text) {
  NYA_Object* root  = nullptr;
  NYA_Error   error = nya_deserialize(arena, (const u8*)text, strlen(text), NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &root);

  nya_assert(error.ok, "the payload is not valid JSON: %s", (NYA_ConstCString)error.message);
  nya_assert(root != nullptr);

  return root;
}

s32 main(void) {
  nya_backtrace_init();
  defer nya_backtrace_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_discord");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the lifecycle refuses what it should and tolerates what it must
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_OFF);
    nya_assert(!nya_discord_connected());

    // Setting presence before init is a caller mistake, not a silent no-op: the activity would be
    // dropped with nothing to say so.
    nya_assert(!nya_discord_activity_set((NYA_DiscordActivity){ .state = "menu" }).ok);

    nya_assert(!nya_discord_init(0).ok, "a zero application id is refused");

    NYA_Error ok = nya_discord_init(123456789012345678ULL);
    nya_assert(ok.ok, "init succeeds even with no Discord running: connecting is the pump's job");
    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_DISCONNECTED, "nothing is connected until the first pump");
    nya_assert(!nya_discord_connected());
    nya_assert(nya_discord_user_name() == nullptr, "nobody is signed in until the handshake completes");

    nya_assert(!nya_discord_init(1).ok, "a second init without a deinit is refused");

    /*
     * Pumping is safe whether or not a client is there, and that is all this can assert.
     *
     * Whether it connects depends on the machine — a developer's desktop has Discord running and CI
     * does not — so pinning the status to DISCONNECTED would be a test that passes only where
     * nobody uses the feature. What must hold everywhere is that the pump never faults and never
     * puts the module back to OFF, which is deinit's answer and nothing else's.
     */
    for (u32 i = 0; i < 8; i++) nya_discord_pump();

    NYA_DiscordStatus status = nya_discord_status();

    nya_assert(status != NYA_DISCORD_STATUS_OFF, "an initialized module never goes back to OFF on its own");
    nya_assert(status < NYA_DISCORD_STATUS_COUNT);

    // Held rather than sent. This is what makes presence work when the player starts Discord later.
    nya_assert(nya_discord_activity_set((NYA_DiscordActivity){ .state = "menu" }).ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a full activity produces valid JSON with every field in place
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordActivity activity = {
      .details      = "Competitive | In a Match",
      .state        = "In a Group",
      .start_time_s = 1520000000,
      .end_time_s   = 1520000323,
      .large_image  = "numbani_map",
      .large_text   = "Numbani",
      .small_image  = "pharah_profile",
      .small_text   = "Pharah",
      .party_id     = "party-id",
      .party_size   = 3,
      .party_max    = 6,
    };

    NYA_String* payload = _nya_discord_activity_payload(arena, &activity);
    NYA_CString text    = nya_string_to_cstring(arena, payload);

    NYA_Object* root = parse(arena, text);

    NYA_Value* command = nya_object_get(root, "cmd");
    nya_assert(command != nullptr && command->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(command->as_string, "SET_ACTIVITY"));

    // The nonce is required, and Discord matches replies by it.
    NYA_Value* nonce = nya_object_get(root, "nonce");
    nya_assert(nonce != nullptr && nonce->type == NYA_TYPE_STRING);

    NYA_Value* args = nya_object_get(root, "args");
    nya_assert(args != nullptr && args->type == NYA_TYPE_OBJECT);

    // The pid is not decoration: Discord watches it and takes the card down when the process exits,
    // which is what stops a crashed game showing as still playing.
    nya_assert(nya_object_get(&args->as_object, "pid") != nullptr, "SET_ACTIVITY carries the process id");

    nya_assert(contains(text, "\"details\":\"Competitive | In a Match\""));
    nya_assert(contains(text, "\"state\":\"In a Group\""));
    nya_assert(contains(text, "\"start\":1520000000"));
    nya_assert(contains(text, "\"end\":1520000323"));
    nya_assert(contains(text, "\"large_image\":\"numbani_map\""));
    nya_assert(contains(text, "\"small_text\":\"Pharah\""));
    nya_assert(contains(text, "\"size\":[3,6]"), "the party count is a two element array, current then maximum");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: absent fields are omitted rather than sent empty
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // An empty string is a *value* to Discord, and it reserves the row for it — so a card with an
    // empty state has a blank line where there should be nothing at all.
    NYA_DiscordActivity sparse = { .details = "Just this" };

    NYA_String* payload = _nya_discord_activity_payload(arena, &sparse);
    NYA_CString text    = nya_string_to_cstring(arena, payload);

    (void)parse(arena, text);

    nya_assert(contains(text, "\"details\":\"Just this\""));
    nya_assert(!contains(text, "\"state\""), "a null field is left out");
    nya_assert(!contains(text, "\"timestamps\""), "and so is the object that would have held nothing");
    nya_assert(!contains(text, "\"assets\""));
    nya_assert(!contains(text, "\"party\""));

    // An empty string is treated the same as absent, so a caller clearing a line by writing "" gets
    // the line removed rather than blanked.
    NYA_DiscordActivity empty_state = { .details = "Just this", .state = "" };

    NYA_String* second = _nya_discord_activity_payload(arena, &empty_state);
    nya_assert(!contains(nya_string_to_cstring(arena, second), "\"state\""));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: everything a player can type is escaped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A quote and a backslash end the string early; a newline and a tab are control characters JSON
    // has no literal form for. Any one of them produces a frame the client answers by hanging up.
    NYA_DiscordActivity hostile = {
      .details = "say \"hi\" \\ now",
      .state   = "line\nbreak\ttab",
    };

    NYA_String* payload = _nya_discord_activity_payload(arena, &hostile);
    NYA_CString text    = nya_string_to_cstring(arena, payload);

    // Parsing is the real assertion: it only succeeds if every one of those was escaped correctly.
    NYA_Object* root = parse(arena, text);

    NYA_Value* args = nya_object_get(root, "args");
    nya_assert(args != nullptr && args->type == NYA_TYPE_OBJECT);

    NYA_Value* inner = nya_object_get(&args->as_object, "activity");
    nya_assert(inner != nullptr && inner->type == NYA_TYPE_OBJECT);

    // And round trips to exactly what went in, so the escaping is not merely valid but faithful.
    NYA_Value* details = nya_object_get(&inner->as_object, "details");
    nya_assert(details != nullptr && details->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(details->as_string, "say \"hi\" \\ now"), "the text survives the round trip unchanged");

    NYA_Value* state = nya_object_get(&inner->as_object, "state");
    nya_assert(state != nullptr && state->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(state->as_string, "line\nbreak\ttab"));

    // A raw control byte has to become \u00XX; JSON has no literal form for it.
    NYA_DiscordActivity control = { .details = "bell\x07here" };

    NYA_String* control_payload = _nya_discord_activity_payload(arena, &control);
    NYA_CString control_text    = nya_string_to_cstring(arena, control_payload);

    (void)parse(arena, control_text);
    nya_assert(contains(control_text, "\\u0007"), "a control character is escaped rather than emitted raw");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a clear is a null activity, not an empty one
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String* payload = _nya_discord_activity_payload(arena, nullptr);
    NYA_CString text    = nya_string_to_cstring(arena, payload);

    (void)parse(arena, text);

    // An empty object would be an activity with no fields, which leaves the card up showing the
    // game's name. Null is what takes it down.
    nya_assert(contains(text, "\"activity\":null"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the party count needs both numbers, and a backwards pair is dropped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordActivity half = { .party_id = "p", .party_size = 3 };

    NYA_CString text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &half));
    (void)parse(arena, text);

    nya_assert(contains(text, "\"id\":\"p\""));
    nya_assert(!contains(text, "\"size\""), "one number is no count; Discord shows nothing for it anyway");

    // Discord rejects the whole activity when the maximum is below the current size, so it is
    // dropped here rather than sent and silently ignored.
    NYA_DiscordActivity backwards = { .party_id = "p", .party_size = 9, .party_max = 2 };

    NYA_CString backwards_text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &backwards));
    (void)parse(arena, backwards_text);
    nya_assert(!contains(backwards_text, "\"size\""));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: buttons, and that secrets win when both are present
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordActivity buttons = {
      .state   = "playing",
      .buttons = { { .label = "Website", .url = "https://example.com" }, { .label = "Discord", .url = "https://discord.gg/x" } },
    };

    NYA_CString text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &buttons));
    (void)parse(arena, text);

    nya_assert(contains(text, "\"buttons\":["));
    nya_assert(contains(text, "\"label\":\"Website\""));
    nya_assert(contains(text, "\"url\":\"https://discord.gg/x\""));

    // A half filled button is dropped: sending one with a label and no url is refused along with the
    // whole activity.
    NYA_DiscordActivity half_button = { .state = "playing", .buttons = { { .label = "Website" } } };

    NYA_CString half_text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &half_button));
    (void)parse(arena, half_text);
    nya_assert(!contains(half_text, "\"buttons\""));

    /*
     * Discord refuses an activity carrying both buttons and secrets, and refuses it silently — the
     * card simply stops updating. The buttons go rather than the secrets, because a join secret is
     * functional and a button is decorative.
     */
    NYA_DiscordActivity both = {
      .party_id    = "p",
      .join_secret = "s3cret",
      .buttons     = { { .label = "Website", .url = "https://example.com" } },
    };

    NYA_CString both_text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &both));
    (void)parse(arena, both_text);

    nya_assert(contains(both_text, "\"join\":\"s3cret\""));
    nya_assert(!contains(both_text, "\"buttons\""), "the buttons are dropped, not the secret");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: change detection compares text, not pointers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The case that matters: a caller formatting into a stack buffer produces a different pointer
    // every frame with identical bytes behind it.
    char first[64];
    char second[64];

    (void)snprintf(first, sizeof(first), "Score: %d", 42);
    (void)snprintf(second, sizeof(second), "Score: %d", 42);

    nya_assert(first != second, "two distinct buffers, so this is a pointer comparison trap");

    NYA_DiscordActivity a = { .details = first, .party_size = 1, .party_max = 4 };
    NYA_DiscordActivity b = { .details = second, .party_size = 1, .party_max = 4 };

    nya_assert(_nya_discord_activity_equals(&a, &b), "same text in different buffers is not a change");

    (void)snprintf(second, sizeof(second), "Score: %d", 43);
    nya_assert(!_nya_discord_activity_equals(&a, &b), "and a real change is seen");

    // Null and empty are the same thing, because the builder omits both — two activities differing
    // only in that produce byte-identical frames and must not be re-sent.
    NYA_DiscordActivity absent = { .state = nullptr };
    NYA_DiscordActivity blank  = { .state = "" };

    nya_assert(_nya_discord_activity_equals(&absent, &blank));

    // Every field participates, not just the first few.
    NYA_DiscordActivity base       = { .details = "d", .state = "s", .large_image = "i", .party_id = "p", .party_size = 1, .party_max = 2 };
    NYA_DiscordActivity changed[6] = { base, base, base, base, base, base };

    changed[0].details    = "other";
    changed[1].state      = "other";
    changed[2].large_image = "other";
    changed[3].party_id   = "other";
    changed[4].party_max  = 3;
    changed[5].end_time_s = 1;

    for (u32 i = 0; i < 6; i++) nya_assert(!_nya_discord_activity_equals(&base, &changed[i]), "field %u is part of the comparison", i);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: overlong text is truncated rather than sent whole
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Discord cuts at 128 bytes anyway. Cutting here keeps the frame valid instead of letting a peer
    // reject the entire activity.
    char long_text[NYA_DISCORD_MAX_TEXT * 4];
    nya_memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';

    NYA_DiscordActivity huge = { .details = long_text };

    NYA_CString text = nya_string_to_cstring(arena, _nya_discord_activity_payload(arena, &huge));

    // Still parses, which is the property that matters: a truncation that cut mid escape would not.
    (void)parse(arena, text);
    nya_assert(strlen(text) < sizeof(long_text), "the payload is shorter than the text handed in");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a dropped connection backs off instead of reconnecting immediately
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The reconnect storm, found by pointing the plugin at a real Discord client.
     *
     * Discord answers an unrecognised application id by *accepting* the socket and then replying
     * `{"code":4000,"message":"Invalid Client ID"}` with a CLOSE frame. That path armed no backoff —
     * only a failed connect() did — so the next pump reconnected at once, and a game shipped with
     * the wrong id opened a socket, wrote a handshake and was hung up on sixty times a second for
     * as long as it ran. Measured against the live client: 180 connect attempts in three seconds
     * before the fix, two after.
     *
     * The backoff is also no longer reset by a successful connect, because opening the socket is not
     * success — the client has not yet said whether it will talk to us. Only READY resets it.
     */
    nya_discord_deinit();
    nya_assert(nya_discord_init(123456789012345678ULL).ok);

    u64 first_delay = _NYA_DISCORD.retry_delay_ms;
    nya_assert(first_delay > 0, "a fresh module starts with a real delay, not zero");

    u64 before_ms = nya_clock_get_monotonic_ms();

    // What a CLOSE frame, a failed write, or a peer hanging up all funnel into.
    _nya_discord_disconnect();

    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_DISCONNECTED, "a drop goes back to retrying, not to OFF");
    nya_assert(_NYA_DISCORD.next_retry_ms >= before_ms + first_delay, "the next attempt is pushed into the future");
    nya_assert(_NYA_DISCORD.retry_delay_ms > first_delay, "and the delay grew");

    // Repeated drops keep growing it, up to the ceiling and no further.
    for (u32 i = 0; i < 32; i++) _nya_discord_disconnect();

    nya_assert(_NYA_DISCORD.retry_delay_ms == _NYA_DISCORD_RETRY_MAX_MS, "the backoff stops at its ceiling rather than overflowing");

    /*
     * And the pump respects it: with the retry armed into the future, nothing reconnects.
     *
     * This is the assertion that actually catches a regression, because it holds whether or not a
     * Discord client is running — the module is in DISCONNECTED with a future deadline either way.
     */
    for (u32 i = 0; i < 16; i++) nya_discord_pump();
    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_DISCONNECTED, "pumping inside the backoff window does not reconnect");

    nya_discord_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: deinit is idempotent and returns the module to its starting state
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_OFF);

    nya_discord_deinit(); // twice, because a game's shutdown path is not always the one it thinks
    nya_assert(nya_discord_status() == NYA_DISCORD_STATUS_OFF);

    nya_assert(!nya_discord_activity_set((NYA_DiscordActivity){ .state = "x" }).ok);
    nya_assert(!nya_discord_activity_clear().ok);

    // And it can be brought back up, which a leaked arena or a stale handle would prevent.
    nya_assert(nya_discord_init(1).ok);
    nya_discord_deinit();
  }

  nya_info("PASSED: test_discord (0 failures)");

  return EXIT_SUCCESS;
}
