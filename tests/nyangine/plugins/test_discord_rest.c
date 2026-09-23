/**
 * The Discord REST client: the rate limit bookkeeping on its own, and the queue driven through canned
 * replies with no network.
 *
 * The headers below are Discord's, copied from a real reply. The clock is a number this file moves, so
 * a bucket that refills in five seconds is proved without waiting five seconds for it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Replies the fake holds before a test has to drain it. */
#define SCRIPT_MAX 8

/** The same fixture as test_discord_gateway.c, and not a token's alphabet for the reason given there. */
#define TEST_TOKEN "not-a-token.not-a-token.this-is-a-test-fixture"

typedef struct {
  u32         status;
  const char* headers;
  const char* body;
} Reply;

typedef struct {
  Reply script[SCRIPT_MAX];
  u32   script_count;
  u32   script_read;

  /** What the last request carried, so a test can look at what actually went out. */
  char last_url[768];
  char last_authorization[256];
  char last_body[1024];
  u32  performed;

  u64 now_ms;
} Fake;

static NYA_Error fake_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
  Fake* fake = (Fake*)user;

  (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", request.url);

  fake->last_authorization[0] = '\0';
  for (u32 i = 0; i < NYA_REQUEST_MAX_HEADERS && request.headers[i].name != nullptr; i++) {
    if (nya_string_equals(request.headers[i].name, "Authorization")) {
      (void)snprintf(fake->last_authorization, sizeof(fake->last_authorization), "%s", request.headers[i].value);
    }
  }

  fake->last_body[0] = '\0';
  if (request.body != nullptr) {
    NYA_String* serialized = nya_serde_json_serialize(arena, request.body, NYA_SERDE_NONE);
    (void)snprintf(fake->last_body, sizeof(fake->last_body), "%s", nya_string_to_cstring(arena, serialized));
  }

  fake->performed += 1;

  nya_assert(fake->script_read < fake->script_count, "the fake ran out of scripted replies");
  const Reply* reply = &fake->script[fake->script_read++];

  *out_response = (NYA_Response){
    .status      = reply->status,
    .raw_body    = nya_string_from(arena, reply->body != nullptr ? reply->body : ""),
    .raw_headers = nya_string_from(arena, reply->headers != nullptr ? reply->headers : ""),
  };

  if (out_response->raw_body->length > 0) {
    NYA_Object* parsed = nullptr;
    if (nya_serde_json_deserialize(arena, out_response->raw_body->items, out_response->raw_body->length, NYA_SERDE_NONE, &parsed).ok) {
      out_response->body = parsed;
    }
  }

  return nya_request_status_is_success(reply->status) ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "POST returned %u", reply->status);
}

static u64 fake_now_ms(void* user) {
  return ((Fake*)user)->now_ms;
}

static void fake_push(Fake* fake, u32 status, const char* headers, const char* body) {
  nya_assert(fake->script_count < SCRIPT_MAX);

  fake->script[fake->script_count++] = (Reply){ .status = status, .headers = headers, .body = body };
}

static NYA_DiscordRestOptions fake_options(Fake* fake) {
  return (NYA_DiscordRestOptions){
    .token          = TEST_TOKEN,
    .application_id = "111222333444555666",
    .perform        = fake_perform,
    .now_ms         = fake_now_ms,
    .user           = fake,
  };
}

/** A reply that never reached a server: what the transport failing looks like to the client. */
static NYA_Response canned(NYA_Arena* arena, u32 status, const char* headers) {
  return (NYA_Response){
    .status      = status,
    .raw_body    = nya_string_create(arena),
    .raw_headers = nya_string_from(arena, headers),
  };
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_discord_rest");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a response header is found by name, whatever case it arrived in
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Response response = canned(
      arena, 200,
      "X-RateLimit-Bucket: abcd1234\n"
      "x-ratelimit-remaining: 4\n"
      "Content-Type: application/json\n"
    );

    char value[64] = { 0 };

    nya_assert(nya_response_header(&response, "x-ratelimit-bucket", value, sizeof(value)));
    nya_assert(nya_string_equals(value, "abcd1234"), "matched without regard to case, as HTTP requires");

    nya_assert(nya_response_header(&response, "X-RATELIMIT-REMAINING", value, sizeof(value)));
    nya_assert(nya_string_equals(value, "4"));

    nya_assert(!nya_response_header(&response, "x-ratelimit-reset-after", value, sizeof(value)), "a header that is not there");
    nya_assert(nya_string_equals(value, ""), "and the buffer is cleared rather than left as it was");

    // Refused rather than truncated: half of a number is a wrong answer where a missing one is not.
    char tiny[4] = { 0 };
    nya_assert(!nya_response_header(&response, "x-ratelimit-bucket", tiny, sizeof(tiny)));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a bucket is learned from a reply and closes when it is spent
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordRateLimit limits = { 0 };
    u64                  wait   = 0;

    // Never seen, so nothing says it is spent: the first call is how a bucket gets learned at all.
    nya_assert(nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 1000, &wait));
    nya_assert_eq(wait, 0ULL);

    NYA_Response spent = canned(
      arena, 200,
      "x-ratelimit-bucket: abcd1234\n"
      "x-ratelimit-limit: 5\n"
      "x-ratelimit-remaining: 0\n"
      "x-ratelimit-reset-after: 2.5\n"
    );

    nya_discord_rate_limit_observe(&limits, "POST /channels/1/messages", 200, &spent, 1000);

    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 1000, &wait), "no calls left in the window");
    nya_assert(wait == 2500ULL, "and the wait is the reset Discord sent, as milliseconds");

    // Another channel is another bucket. A busy channel must not throttle a quiet one.
    nya_assert(nya_discord_rate_limit_ready(&limits, "POST /channels/2/messages", 1000, &wait));

    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 3499, &wait));
    nya_assert(nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 3500, &wait), "the window passed, so the server refilled it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a 429 closes its own bucket, and a global one closes everything
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordRateLimit limits = { 0 };
    u64                  wait   = 0;

    NYA_Response limited = canned(arena, 429, "x-ratelimit-bucket: abcd1234\nretry-after: 1.2\n");
    nya_discord_rate_limit_observe(&limits, "POST /channels/1/messages", 429, &limited, 1000);

    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 1000, &wait));
    nya_assert(wait == 1200ULL, "Retry-After is what the server says about this bucket now");
    nya_assert(nya_discord_rate_limit_ready(&limits, "POST /applications/{id}/commands", 1000, &wait), "and only about this bucket");

    // The one Discord escalates to a ban when a client keeps pushing through it, so it stops everything
    // rather than one route.
    NYA_Response global = canned(arena, 429, "x-ratelimit-global: true\nretry-after: 10\n");
    nya_discord_rate_limit_observe(&limits, "POST /applications/{id}/commands", 429, &global, 2000);

    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /applications/{id}/commands", 2000, &wait));
    nya_assert(wait == 10000ULL);
    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/9/messages", 2000, &wait), "a route this client has never touched, too");

    nya_assert(nya_discord_rate_limit_ready(&limits, "POST /channels/9/messages", 12000, &wait), "and it lifts when it said it would");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a reply with no rate limit headers leaves the bucket where it was
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_DiscordRateLimit limits = { 0 };
    u64                  wait   = 0;

    NYA_Response spent = canned(arena, 200, "x-ratelimit-bucket: abcd1234\nx-ratelimit-remaining: 0\nx-ratelimit-reset-after: 5\n");
    nya_discord_rate_limit_observe(&limits, "POST /channels/1/messages", 200, &spent, 1000);
    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 1000, &wait));

    // A reply that says nothing about the limit says nothing about the limit. Reading it as a refill
    // would reopen a bucket the server never said was open.
    NYA_Response bare = canned(arena, 200, "content-type: application/json\n");
    nya_discord_rate_limit_observe(&limits, "POST /channels/1/messages", 200, &bare, 2000);

    nya_assert(!nya_discord_rate_limit_ready(&limits, "POST /channels/1/messages", 2000, &wait));
    nya_assert(wait == 4000ULL, "and the window it was in is still the one that counts");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a call that cannot work is refused where it is made
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;

    NYA_Error no_token = nya_discord_rest_create(arena, (NYA_DiscordRestOptions){ .perform = fake_perform, .user = &fake }, &rest);
    nya_assert(no_token.kind == NYA_ERROR_INVALID_ARGUMENT);

    NYA_Error quoted = nya_discord_rest_create(arena, (NYA_DiscordRestOptions){ .token = "abc\"def" }, &rest);
    nya_assert(quoted.kind == NYA_ERROR_INVALID_ARGUMENT);
    nya_assert(!nya_string_contains((NYA_ConstCString)quoted.message, "abc"), "the token never reaches an error message");

    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;

    nya_assert(nya_discord_rest_message_send(rest, "", "hello", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "a message needs a channel");
    nya_assert(nya_discord_rest_message_send(rest, "42", "", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "and something to say");

    // Refused rather than cut: cutting UTF-8 mid codepoint makes a body Discord rejects for a reason
    // the caller cannot see.
    char long_content[NYA_DISCORD_REST_MAX_CONTENT + 8] = { 0 };
    for (u32 i = 0; i < sizeof(long_content) - 1; i++) long_content[i] = 'a';
    nya_assert(nya_discord_rest_message_send(rest, "42", long_content, &id).kind == NYA_ERROR_INVALID_ARGUMENT);

    nya_assert(nya_discord_rest_command_register(rest, "ping", "", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "Discord refuses a command with no description");
    nya_assert(nya_discord_rest_interaction_reply(rest, "1", "", "pong", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "and a reply with no interaction token");

    nya_assert(nya_discord_rest_pending(rest) == 0U, "none of them were queued");
    nya_assert_eq(fake.performed, 0U);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a message goes out as Discord expects it, with the bot scheme and no token in the result
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    NYA_EXPECT(nya_discord_rest_message_send(rest, "777", "pong", &id));
    nya_assert(id > 0, "a queued request has a handle its result carries back");
    nya_assert_eq(nya_discord_rest_pending(rest), 1U);

    fake_push(&fake, 200, "x-ratelimit-bucket: abcd1234\nx-ratelimit-remaining: 4\nx-ratelimit-reset-after: 5\n", "{\"id\":\"999\"}");

    NYA_DiscordRestResult result = { 0 };
    nya_assert(nya_discord_rest_poll(rest, &result));

    nya_assert(result.error.ok);
    nya_assert_eq(result.id, id);
    nya_assert_eq(result.status, 200U);
    nya_assert(nya_string_equals(result.route, "POST /channels/777/messages"));
    nya_assert(result.body != nullptr, "and the reply is decoded");
    nya_assert_eq(nya_discord_rest_pending(rest), 0U);

    nya_assert(nya_string_equals(fake.last_url, NYA_DISCORD_REST_URL "/channels/777/messages"));
    nya_assert(nya_string_equals(fake.last_authorization, "Bot " TEST_TOKEN), "the bot scheme, not the bearer one");
    nya_assert(nya_string_equals(fake.last_body, "{\"content\":\"pong\"}"));

    // What a log line would print. The route is deliberately the shape with no ids or tokens in it.
    nya_assert(!nya_string_contains(result.route, TEST_TOKEN));

    nya_assert(!nya_discord_rest_poll(rest, &result), "an empty queue performs nothing");
    nya_assert_eq(fake.performed, 1U);

    // The bucket that reply described is the client's now, and is what a caller can look at.
    const NYA_DiscordRateLimit* limits = nya_discord_rest_limits(rest);
    nya_assert_eq(limits->bucket_count, 1U);
    nya_assert_eq(limits->buckets[0].remaining, 4U);
    nya_assert(nya_string_equals((NYA_ConstCString)limits->buckets[0].id, "abcd1234"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a slash command and an interaction reply, which is a whole bot's worth of REST
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    NYA_EXPECT(nya_discord_rest_command_register(rest, "ping", "Answer with pong", &id));

    fake_push(&fake, 201, "x-ratelimit-bucket: cmds\nx-ratelimit-remaining: 1\nx-ratelimit-reset-after: 1\n", "{\"id\":\"1\"}");

    NYA_DiscordRestResult result = { 0 };
    nya_assert(nya_discord_rest_poll(rest, &result));
    nya_assert(result.error.ok);
    nya_assert(result.kind == NYA_DISCORD_REST_COMMAND_REGISTER);
    nya_assert(nya_string_equals(fake.last_url, NYA_DISCORD_REST_URL "/applications/111222333444555666/commands"));
    nya_assert(nya_string_contains(fake.last_body, "\"name\":\"ping\""));
    nya_assert(nya_string_contains(fake.last_body, "\"type\":1"), "type 1 is CHAT_INPUT, which is what a slash command is");

    NYA_EXPECT(nya_discord_rest_interaction_reply(rest, "555", "interaction-token-abc", "pong", &id));

    fake_push(&fake, 204, "x-ratelimit-bucket: callbacks\nx-ratelimit-remaining: 4\nx-ratelimit-reset-after: 1\n", nullptr);
    nya_assert(nya_discord_rest_poll(rest, &result));
    nya_assert(result.error.ok);
    nya_assert(nya_string_equals(fake.last_url, NYA_DISCORD_REST_URL "/interactions/555/interaction-token-abc/callback"));
    nya_assert(nya_string_contains(fake.last_body, "\"type\":4"), "type 4 is a message the user sees");
    nya_assert(nya_string_contains(fake.last_body, "\"content\":\"pong\""));

    // The interaction token is a credential for that one interaction, so it is in the url and nowhere a
    // log line would reach.
    nya_assert(nya_string_equals(result.route, "POST /interactions/{id}/{token}/callback"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a 429 is waited out rather than retried into the ground
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    NYA_EXPECT(nya_discord_rest_message_send(rest, "777", "pong", &id));

    fake_push(&fake, 429, "x-ratelimit-bucket: abcd1234\nretry-after: 3\n", "{\"message\":\"You are being rate limited.\"}");
    fake_push(&fake, 200, "x-ratelimit-bucket: abcd1234\nx-ratelimit-remaining: 4\nx-ratelimit-reset-after: 5\n", "{\"id\":\"999\"}");

    NYA_DiscordRestResult result = { 0 };
    nya_assert(!nya_discord_rest_poll(rest, &result), "a 429 is not an answer for the caller, it is a wait");
    nya_assert(nya_discord_rest_pending(rest) == 1U, "so the request is still queued");
    nya_assert_eq(fake.performed, 1U);

    // Polled as often as the caller likes, and nothing goes out: this is the part that keeps a token off
    // Discord's bad list, and it costs nothing to ask.
    for (u32 i = 0; i < 100; i++) nya_assert(!nya_discord_rest_poll(rest, &result));
    nya_assert(fake.performed == 1U, "not one request while the bucket was closed");

    fake.now_ms += 3000;
    nya_assert(nya_discord_rest_poll(rest, &result));
    nya_assert(result.error.ok);
    nya_assert_eq(fake.performed, 2U);
    nya_assert_eq(nya_discord_rest_pending(rest), 0U);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a request that keeps being refused is given back rather than retried forever
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    NYA_EXPECT(nya_discord_rest_message_send(rest, "777", "pong", &id));

    for (u32 i = 0; i < NYA_DISCORD_REST_MAX_ATTEMPTS; i++) fake_push(&fake, 500, "", "");

    NYA_DiscordRestResult result = { 0 };
    for (u32 i = 0; i + 1 < NYA_DISCORD_REST_MAX_ATTEMPTS; i++) {
      nya_assert(!nya_discord_rest_poll(rest, &result), "a 5xx is Discord's problem and worth another try");

      // A 5xx carries no Retry-After, so the wait is this client's own and the clock has to pass it: a
      // retry that went out on the next poll would be a bot hammering an API that is already struggling.
      nya_assert(!nya_discord_rest_poll(rest, &result), "and not one millisecond later");
      fake.now_ms += 10000;
    }

    nya_assert(nya_discord_rest_poll(rest, &result));
    nya_assert(!result.error.ok, "past the allowance it comes back as a failure");
    nya_assert_eq(result.status, 500U);
    nya_assert_eq(nya_discord_rest_pending(rest), 0U);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a full queue is a refusal at the call, not a growing allocation
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    for (u32 i = 0; i < NYA_DISCORD_REST_MAX_QUEUE; i++) NYA_EXPECT(nya_discord_rest_message_send(rest, "777", "pong", &id));

    NYA_Error full = nya_discord_rest_message_send(rest, "777", "pong", &id);
    nya_assert(full.kind == NYA_ERROR_OUT_OF_MEMORY, "a bot this far behind is asking for more than Discord will give it");
    nya_assert(id == 0ULL, "and it gets no handle for a request that was not taken");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: one closed bucket does not hold up a request that shares nothing with it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Fake             fake = { .now_ms = 1000 };
    NYA_DiscordRest* rest = nullptr;
    NYA_EXPECT(nya_discord_rest_create(arena, fake_options(&fake), &rest));
    defer nya_discord_rest_destroy(rest);

    u64 id = 0;
    NYA_EXPECT(nya_discord_rest_message_send(rest, "777", "one", &id));
    fake_push(&fake, 429, "x-ratelimit-bucket: abcd1234\nretry-after: 30\n", "");

    NYA_DiscordRestResult result = { 0 };
    nya_assert(!nya_discord_rest_poll(rest, &result));

    // Queued behind a channel that is now shut for half a minute, and it goes out anyway.
    NYA_EXPECT(nya_discord_rest_command_register(rest, "ping", "Answer with pong", &id));
    fake_push(&fake, 201, "x-ratelimit-bucket: cmds\nx-ratelimit-remaining: 1\nx-ratelimit-reset-after: 1\n", "{\"id\":\"1\"}");

    nya_assert(nya_discord_rest_poll(rest, &result));
    nya_assert(result.kind == NYA_DISCORD_REST_COMMAND_REGISTER);
    nya_assert(nya_discord_rest_pending(rest) == 1U, "the message is still waiting on its own bucket");
  }

  printf("PASSED: test_discord_rest\n");
  return 0;
}
