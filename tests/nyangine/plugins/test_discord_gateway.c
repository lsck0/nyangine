/**
 * The Discord gateway state machine, driven through the transport seam with no socket anywhere.
 *
 * Every frame below is one Discord really sends, typed out by hand. The clock is a number this file
 * moves, which is the only way to prove a heartbeat fires forty five seconds in without waiting forty
 * five seconds, and the jitter is a constant so that a scheduled time is a number a test can state.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Frames the script holds before a test has to drain it. Every scenario here uses a handful. */
#define SCRIPT_MAX 16

/** Payloads the fake remembers having sent. A login, a heartbeat and a little room. */
#define SENT_MAX 16

/** Longest payload the fake keeps. An IDENTIFY with a full length token is well under this. */
#define SENT_BYTES 1024

/*
 * A token with a real one's shape — three dot separated parts, nothing that needs escaping — and
 * deliberately not its alphabet: a string that looks enough like a bot token is refused by a secret
 * scanner before it reaches a remote, and a test fixture is not worth teaching people to bypass one.
 */
#define TEST_TOKEN "not-a-token.not-a-token.this-is-a-test-fixture"

typedef struct {
  NYA_WebSocketEventKind kind;
  const char*            text;
  NYA_WebSocketClose     code;
  const char*            reason;
} Frame;

/** The transport the gateway thinks it is talking to. */
typedef struct {
  Frame script[SCRIPT_MAX];
  u32   script_count;
  u32   script_read;

  char sent[SENT_MAX][SENT_BYTES];
  u32  sent_count;

  u32  opens;
  u32  closes;
  char last_url[256];

  /** Set to make the next open fail, which is what an unreachable gateway looks like from here. */
  b8 open_fails;

  u64 now_ms;
  f32 jitter;
} Fake;

/*
 * ─────────────────────────────────────────────────────────
 * THE FAKE TRANSPORT
 * ─────────────────────────────────────────────────────────
 */

static NYA_Error fake_open(void* user, NYA_ConstCString url) {
  Fake* fake = (Fake*)user;

  if (fake->open_fails) return nya_error(NYA_ERROR_NOT_FOUND, "the test refused this connect");

  fake->opens += 1;
  (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", url);

  return NYA_OK;
}

static void fake_close(void* user) {
  Fake* fake = (Fake*)user;

  fake->closes += 1;

  // A closed connection has no backlog: anything scripted and unread belonged to the socket that just
  // went away, which is what makes a reconnect scenario start from an empty script.
  fake->script_count = 0;
  fake->script_read  = 0;
}

static b8 fake_poll(void* user, OUT NYA_WebSocketEvent* out_event) {
  Fake* fake = (Fake*)user;

  if (fake->script_read >= fake->script_count) return false;

  const Frame* frame = &fake->script[fake->script_read++];

  *out_event = (NYA_WebSocketEvent){
    .kind   = frame->kind,
    .data   = (const u8*)frame->text,
    .size   = frame->text != nullptr ? strlen(frame->text) : 0,
    .code   = frame->code,
    .reason = frame->reason != nullptr ? frame->reason : "",
  };

  return true;
}

static NYA_Error fake_send(void* user, const char* text, u64 size) {
  Fake* fake = (Fake*)user;

  nya_assert(fake->sent_count < SENT_MAX, "the fake ran out of room for sent payloads");
  nya_assert(size < SENT_BYTES, "the fake ran out of room for one payload");

  nya_memcpy(fake->sent[fake->sent_count], text, size);
  fake->sent[fake->sent_count][size] = '\0';
  fake->sent_count += 1;

  return NYA_OK;
}

static u64 fake_now_ms(void* user) {
  return ((Fake*)user)->now_ms;
}

static f32 fake_jitter(void* user) {
  return ((Fake*)user)->jitter;
}

static NYA_DiscordGatewayTransport fake_transport(Fake* fake) {
  return (NYA_DiscordGatewayTransport){
    .user   = fake,
    .open   = fake_open,
    .close  = fake_close,
    .poll   = fake_poll,
    .send   = fake_send,
    .now_ms = fake_now_ms,
    .jitter = fake_jitter,
  };
}

/** Scripts one text payload for the next poll to read. */
static void fake_push(Fake* fake, const char* text) {
  nya_assert(fake->script_count < SCRIPT_MAX);

  fake->script[fake->script_count++] = (Frame){ .kind = NYA_WEBSOCKET_EVENT_TEXT, .text = text };
}

/** Scripts the connection ending with `code`. */
static void fake_push_close(Fake* fake, NYA_WebSocketClose code, const char* reason) {
  nya_assert(fake->script_count < SCRIPT_MAX);

  fake->script[fake->script_count++] = (Frame){ .kind = NYA_WEBSOCKET_EVENT_CLOSED, .code = code, .reason = reason };
}

/** The last thing the gateway sent, or "" when it has sent nothing. */
static const char* fake_last_sent(const Fake* fake) {
  return fake->sent_count == 0 ? "" : fake->sent[fake->sent_count - 1];
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_discord_gateway");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the close code table, which is the one thing that must not be wrong twice
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The whole point of the component. A bot that retries any of these has its token disabled by
    // Discord, so each one is named rather than covered by a range.
    nya_assert(nya_discord_gateway_close_action(4004) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);
    nya_assert(nya_discord_gateway_close_action(4010) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);
    nya_assert(nya_discord_gateway_close_action(4011) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);
    nya_assert(nya_discord_gateway_close_action(4012) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);
    nya_assert(nya_discord_gateway_close_action(4013) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);
    nya_assert(nya_discord_gateway_close_action(4014) == NYA_DISCORD_GATEWAY_CLOSE_FATAL);

    // A connection that broke rather than a session that ended, so the events since the last sequence
    // are still there to be replayed.
    nya_assert(nya_discord_gateway_close_action(1006) == NYA_DISCORD_GATEWAY_CLOSE_RESUME);
    nya_assert(nya_discord_gateway_close_action(4000) == NYA_DISCORD_GATEWAY_CLOSE_RESUME);
    nya_assert(nya_discord_gateway_close_action(4008) == NYA_DISCORD_GATEWAY_CLOSE_RESUME);

    // The session is gone: resuming it would only be refused again.
    nya_assert(nya_discord_gateway_close_action(1000) == NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY);
    nya_assert(nya_discord_gateway_close_action(4007) == NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY);
    nya_assert(nya_discord_gateway_close_action(4009) == NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY);

    // A code Discord has not invented yet. Failing towards a fresh login costs a round trip; failing
    // towards a resume would cost the events in between.
    nya_assert(nya_discord_gateway_close_action(4099) == NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the backoff doubles and then stops doubling
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert_eq(nya_discord_gateway_backoff_ms(0), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS);
    nya_assert_eq(nya_discord_gateway_backoff_ms(1), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS * 2);
    nya_assert_eq(nya_discord_gateway_backoff_ms(2), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS * 4);
    nya_assert_eq(nya_discord_gateway_backoff_ms(5), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS * 32);

    // Held at the ceiling, and still held there for an attempt count that would overflow a naive shift.
    nya_assert_eq(nya_discord_gateway_backoff_ms(6), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS);
    nya_assert_eq(nya_discord_gateway_backoff_ms(40), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS);
    nya_assert_eq(nya_discord_gateway_backoff_ms(4000000000U), (u64)NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a token that is not one is refused before anything is dialled
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake                fake    = { .jitter = 0.5F };
    NYA_DiscordGateway* gateway = nullptr;

    NYA_Error no_token = nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .transport = fake_transport(&fake) }, &gateway
    );
    nya_assert(no_token.kind == NYA_ERROR_INVALID_ARGUMENT, "a gateway with no token is a caller mistake");

    // A quote would close the JSON string the token goes into, which is the whole reason the check is
    // there. The message says what was wrong and does not quote the thing that was wrong.
    NYA_Error quoted = nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = "abc\"def", .transport = fake_transport(&fake) }, &gateway
    );
    nya_assert(quoted.kind == NYA_ERROR_INVALID_ARGUMENT);
    nya_assert(!nya_string_contains((NYA_ConstCString)quoted.message, "abc"), "the token never reaches an error message");

    NYA_Error bad_shard = nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .shard_id = 4, .shard_count = 4, .transport = fake_transport(&fake) }, &gateway
    );
    nya_assert(bad_shard.kind == NYA_ERROR_INVALID_ARGUMENT, "shard 4 of 4 does not exist");

    nya_assert(fake.opens == 0U, "nothing was dialled for any of them");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hello, identify, heartbeat, ack, and then a resume after the peer goes quiet
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena,
      (NYA_DiscordGatewayOptions){
        .token     = TEST_TOKEN,
        .intents   = NYA_DISCORD_INTENT_GUILDS | NYA_DISCORD_INTENT_GUILD_MESSAGES,
        .transport = fake_transport(&fake),
      },
      &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    nya_assert(fake.opens == 1U, "creating dials at once");
    nya_assert(nya_string_equals(fake.last_url, NYA_DISCORD_GATEWAY_URL));
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_CONNECTING);
    nya_assert(nya_string_equals(nya_discord_gateway_state_name(NYA_DISCORD_GATEWAY_STATE_CONNECTING), "connecting"));
    nya_assert(nya_discord_gateway_sequence(gateway) == (s64)-1, "no payload has carried a sequence yet");

    /*
     * HELLO. The interval arms the heartbeat and the login goes out in the same poll.
     */
    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");

    NYA_DiscordGatewayEvent event = { 0 };
    nya_assert(!nya_discord_gateway_poll(gateway, &event), "a HELLO is the client's business, not the caller's");

    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_IDENTIFYING);
    nya_assert_eq(fake.sent_count, 1U);
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"op\":2"), "the first thing sent is an IDENTIFY");
    nya_assert(nya_string_contains(fake_last_sent(&fake), TEST_TOKEN), "which carries the token");
    nya_assert(
      nya_string_contains(fake_last_sent(&fake), "\"intents\":513"),
      "and the intents mask as one number: GUILDS is bit 0 and GUILD_MESSAGES is bit 9"
    );
    nya_assert(!nya_string_contains(fake_last_sent(&fake), "shard"), "an unsharded bot does not send a shard");

    /*
     * READY: the session and the resume url are what a RESUME later needs.
     */
    fake_push(&fake,
              "{\"op\":0,\"s\":1,\"t\":\"READY\",\"d\":{\"session_id\":\"session-one\","
              "\"resume_gateway_url\":\"wss://gateway-us-east1-b.discord.gg\",\"user\":{\"username\":\"nyabot\"}}}");

    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_READY);
    nya_assert(nya_string_equals(event.name, "nyabot"), "READY names the bot user");
    nya_assert(event.data != nullptr, "and hands the whole payload over");
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_READY);
    nya_assert_eq(nya_discord_gateway_sequence(gateway), (s64)1);

    /*
     * The first heartbeat, jittered. With a jitter of a half it is due half an interval in, and not one
     * millisecond before: the whole point of the jitter is that this is not the same for every bot.
     */
    fake.now_ms += 22499;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(fake.sent_count == 1U, "the first heartbeat is not due yet");

    fake.now_ms += 1;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert_eq(fake.sent_count, 2U);
    nya_assert(nya_string_equals(fake_last_sent(&fake), "{\"op\":1,\"d\":1}"), "a heartbeat carries the last sequence");

    // Acknowledged, so the connection is alive and the next beat is a whole interval away.
    fake_push(&fake, "{\"op\":11,\"d\":null}");
    nya_assert(!nya_discord_gateway_poll(gateway, &event));

    fake.now_ms += 45000;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(fake.sent_count == 3U, "the second heartbeat is a full interval after the first");

    /*
     * This one goes unanswered. A socket that is open and silent is the case a heartbeat exists to catch,
     * so the next due beat drops the connection instead of sending into it.
     */
    fake.now_ms += 45000;
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);
    nya_assert(event.code == (u16)4000, "not 1000, which would throw the session away");
    nya_assert(event.retry_in_ms > 0, "and it says when it will come back");
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_IDLE);
    nya_assert_eq(fake.closes, 1U);
    nya_assert(fake.sent_count == 3U, "nothing was sent into the dead socket");

    // Kept, because a poll that reports nothing still clears the event it was handed.
    u64 retry_in_ms = event.retry_in_ms;

    // Nothing happens before the backoff has passed.
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert_eq(fake.opens, 1U);

    fake.now_ms += retry_in_ms;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(fake.opens == 2U, "and then it dials again");
    nya_assert(
      nya_string_equals(fake.last_url, "wss://gateway-us-east1-b.discord.gg/?v=10&encoding=json"),
      "on the resume url READY gave, with the version and encoding this client can read"
    );

    /*
     * A second HELLO, and this time the login is a RESUME rather than an IDENTIFY.
     */
    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
    nya_assert(!nya_discord_gateway_poll(gateway, &event));

    nya_assert_eq(fake.sent_count, 4U);
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"op\":6"), "a RESUME, not a second IDENTIFY");
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"session_id\":\"session-one\""));
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"seq\":1"), "replayed from the last sequence seen");

    fake_push(&fake, "{\"op\":0,\"s\":2,\"t\":\"RESUMED\",\"d\":{}}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_RESUMED);
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_READY);

    /*
     * An ordinary dispatch, which is everything a bot is actually there for.
     */
    fake_push(&fake, "{\"op\":0,\"s\":3,\"t\":\"MESSAGE_CREATE\",\"d\":{\"content\":\"!ping\",\"channel_id\":\"42\"}}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISPATCH);
    nya_assert(nya_string_equals(event.name, "MESSAGE_CREATE"));
    nya_assert_eq(nya_discord_gateway_sequence(gateway), (s64)3);

    NYA_Value* content = nya_object_get(event.data, "content");
    nya_assert(content != nullptr && content->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(content->as_string, "!ping"), "the payload arrives decoded");

    /*
     * A payload of the caller's own, which is only legal once logged in.
     */
    NYA_Object* presence = nya_object_create(arena);
    nya_object_set(presence, "op", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 3 });

    NYA_EXPECT(nya_discord_gateway_send(gateway, arena, presence));
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"op\":3"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a rejected token ends the client rather than being retried
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");

    NYA_DiscordGatewayEvent event = { 0 };
    nya_assert(!nya_discord_gateway_poll(gateway, &event));

    // 4014: the bot asked for an intent its application page does not have enabled. Reconnecting cannot
    // enable it, and a client that keeps asking gets the token disabled.
    fake_push_close(&fake, (NYA_WebSocketClose)4014, "Disallowed intent(s).");

    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_FATAL);
    nya_assert_eq(event.code, (u16)4014);
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_FATAL);

    u32 opens_at_refusal = fake.opens;

    // However long the caller keeps polling, and however far the clock moves.
    for (u32 i = 0; i < 8; i++) {
      fake.now_ms += 600000;
      nya_assert(!nya_discord_gateway_poll(gateway, &event), "a fatal gateway produces nothing");
    }

    nya_assert(fake.opens == opens_at_refusal, "and never dials again");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a close that is not fatal is retried, and the waits grow
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Jitter of one, so the wait is the whole delay and the arithmetic below is exact: half the backoff
    // plus a full half is the backoff.
    Fake fake = { .jitter = 1.0F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };

    for (u32 attempt = 0; attempt < 4; attempt++) {
      fake_push_close(&fake, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");

      nya_assert(nya_discord_gateway_poll(gateway, &event));
      nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);
      nya_assert_eq(event.retry_in_ms, nya_discord_gateway_backoff_ms(attempt));

      fake.now_ms += event.retry_in_ms;
      nya_assert(!nya_discord_gateway_poll(gateway, &event));
      nya_assert(fake.opens == attempt + 2, "each wait ends in another attempt");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: giving up after a fixed number of attempts, for a bot that is not meant to stay up
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena,
      (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .max_reconnect_attempts = 2, .transport = fake_transport(&fake) },
      &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };

    for (u32 attempt = 0; attempt < 2; attempt++) {
      fake_push_close(&fake, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");
      nya_assert(nya_discord_gateway_poll(gateway, &event));
      nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);

      fake.now_ms += event.retry_in_ms;
      nya_assert(!nya_discord_gateway_poll(gateway, &event));
    }

    fake_push_close(&fake, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_FATAL, "the third failure is past the allowance");
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_FATAL);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an invalid session throws the session away and logs in afresh
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };

    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
    fake_push(&fake, "{\"op\":0,\"s\":7,\"t\":\"READY\",\"d\":{\"session_id\":\"session-two\",\"user\":{\"username\":\"nyabot\"}}}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_READY);

    // `d` is a bare false here, not an object: the session cannot be resumed.
    fake_push(&fake, "{\"op\":9,\"d\":false}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);
    nya_assert(event.retry_in_ms >= 1000 && event.retry_in_ms <= 5000, "Discord's one to five seconds, not the backoff");
    nya_assert(nya_discord_gateway_sequence(gateway) == (s64)-1, "and the sequence went with the session");

    fake.now_ms += event.retry_in_ms;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(nya_string_equals(fake.last_url, NYA_DISCORD_GATEWAY_URL), "back to the front door, not a resume url");

    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(nya_string_contains(fake_last_sent(&fake), "\"op\":2"), "and a fresh IDENTIFY");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the server asking for a heartbeat, and asking for a reconnect
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.9F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };

    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
    fake_push(&fake, "{\"op\":0,\"s\":4,\"t\":\"READY\",\"d\":{\"session_id\":\"session-three\",\"user\":{\"username\":\"nyabot\"}}}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));

    u32 sent_before = fake.sent_count;

    // Opcode 1 from the server, which it uses when it is about to go away and wants to know who is still
    // there. It is answered at once rather than at the next scheduled beat.
    fake_push(&fake, "{\"op\":1,\"d\":null}");
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert_eq(fake.sent_count, sent_before + 1);
    nya_assert(nya_string_equals(fake_last_sent(&fake), "{\"op\":1,\"d\":4}"));

    // Opcode 7: come back, and the session is still good.
    fake_push(&fake, "{\"op\":7,\"d\":null}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);
    nya_assert(event.code == (u16)4000, "resumable, so the session survives it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a gateway that cannot be dialled is a retry, not a failure at create
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000, .open_fails = true };

    NYA_DiscordGateway* gateway = nullptr;

    // The ordinary state of a machine that has just booted with no route yet. Failing here would make
    // every caller write the retry themselves.
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };
    nya_assert(!nya_discord_gateway_poll(gateway, &event), "nothing happens before the first backoff has passed");

    fake.now_ms += NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS;
    nya_assert(nya_discord_gateway_poll(gateway, &event));
    nya_assert(event.kind == NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED);

    fake.open_fails = false;
    fake.now_ms    += event.retry_in_ms;
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(fake.opens == 1U, "and it connects once the network is back");
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_CONNECTING);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a payload the client cannot read does not end the session
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake fake = { .jitter = 0.5F, .now_ms = 1000 };

    NYA_DiscordGateway* gateway = nullptr;
    NYA_EXPECT(nya_discord_gateway_create(
      arena, (NYA_DiscordGatewayOptions){ .token = TEST_TOKEN, .transport = fake_transport(&fake) }, &gateway
    ));
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordGatewayEvent event = { 0 };

    fake_push(&fake, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");
    fake_push(&fake, "{\"op\":0,\"s\":9,\"t\":\"READY\",\"d\":{\"session_id\":\"session-four\",\"user\":{\"username\":\"nyabot\"}}}");
    nya_assert(nya_discord_gateway_poll(gateway, &event));

    // Truncated by a proxy, or an opcode from a version of the protocol this does not know. Neither is a
    // reason to throw away a session and replay from a sequence.
    fake_push(&fake, "{\"op\":0,\"s\":10,\"t\":");
    fake_push(&fake, "{\"op\":4242,\"d\":{}}");
    nya_assert(!nya_discord_gateway_poll(gateway, &event));
    nya_assert(nya_discord_gateway_state(gateway) == NYA_DISCORD_GATEWAY_STATE_READY);
    nya_assert_eq(fake.closes, 0U);
  }

  printf("PASSED: test_discord_gateway\n");
  return 0;
}
