/**
 * The Twitch bot's two halves, with no network: the socket driven through scripted frames and a clock
 * this file moves, and the Helix client through canned replies.
 *
 * What is proved here is the behaviour a bot depends on and cannot test against Twitch: that every
 * welcome is a new session to subscribe against, that silence is what ends a socket, that a reconnect
 * loses nothing, and that a message delivered twice is handed over once.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Frames a socket slot holds before a poll has to drain it. */
#define FRAMES_MAX 8

/** Replies the Helix fake holds. */
#define SCRIPT_MAX 8

/** Shaped like the real thing and deliberately not one. */
#define TEST_TOKEN "not-a-token-this-is-a-test-fixture"

/*
 * ─────────────────────────────────────────────────────────
 * THE SOCKET
 * ─────────────────────────────────────────────────────────
 */

typedef struct {
  NYA_WebSocketEventKind kind;
  const char*            text;
} Frame;

typedef struct {
  b8    open;
  char  url[256];
  Frame frames[FRAMES_MAX];
  u32   frame_count;
  u32   frame_read;
} Slot;

typedef struct {
  Slot slots[2];

  /** How many times each slot was opened and closed, so a move can be checked rather than assumed. */
  u32 opens[2];
  u32 closes[2];

  /** Set to make the next open fail, for the url that will never work. */
  b8 refuse_open;

  u64 now_ms;
  u64 now_s;
} Sockets;

static NYA_Error socket_open(void* user, u32 slot, NYA_ConstCString url) {
  Sockets* sockets = (Sockets*)user;

  if (sockets->refuse_open) return nya_error(NYA_ERROR_NOT_OK, "that url will not open");

  sockets->slots[slot] = (Slot){ .open = true };
  (void)snprintf(sockets->slots[slot].url, sizeof(sockets->slots[slot].url), "%s", url);

  sockets->opens[slot] += 1;

  return NYA_OK;
}

static void socket_close(void* user, u32 slot) {
  Sockets* sockets = (Sockets*)user;

  if (!sockets->slots[slot].open) return;

  sockets->slots[slot]   = (Slot){ 0 };
  sockets->closes[slot] += 1;
}

static b8 socket_poll(void* user, u32 slot, OUT NYA_WebSocketEvent* out_event) {
  Sockets* sockets = (Sockets*)user;
  Slot*    live    = &sockets->slots[slot];

  if (!live->open || live->frame_read >= live->frame_count) return false;

  const Frame* frame = &live->frames[live->frame_read++];

  *out_event = (NYA_WebSocketEvent){
    .kind   = frame->kind,
    .data   = (const u8*)frame->text,
    .size   = frame->text != nullptr ? strlen(frame->text) : 0,
    .reason = frame->kind == NYA_WEBSOCKET_EVENT_CLOSED ? "the peer went away" : "",
  };

  return true;
}

static u64 socket_now_ms(void* user) {
  return ((Sockets*)user)->now_ms;
}

static u64 socket_now_s(void* user) {
  return ((Sockets*)user)->now_s;
}

static void frame_push(Sockets* sockets, u32 slot, NYA_WebSocketEventKind kind, const char* text) {
  Slot* live = &sockets->slots[slot];

  nya_assert(live->frame_count < FRAMES_MAX);

  live->frames[live->frame_count++] = (Frame){ .kind = kind, .text = text };
}

static NYA_TwitchEventSubOptions socket_options(Sockets* sockets) {
  return (NYA_TwitchEventSubOptions){
    .transport = {
      .user   = sockets,
      .open   = socket_open,
      .close  = socket_close,
      .poll   = socket_poll,
      .now_ms = socket_now_ms,
      .now_s  = socket_now_s,
    },
  };
}

/** Twitch's own message shapes, with the timestamp left off where the replay window is not the point. */
static const char* WELCOME = "{\"metadata\":{\"message_id\":\"w-1\",\"message_type\":\"session_welcome\"},"
                             "\"payload\":{\"session\":{\"id\":\"session-abc\",\"keepalive_timeout_seconds\":10}}}";

static const char* KEEPALIVE = "{\"metadata\":{\"message_id\":\"k-1\",\"message_type\":\"session_keepalive\"},\"payload\":{}}";

static const char* CHAT = "{\"metadata\":{\"message_id\":\"n-1\",\"message_type\":\"notification\",\"subscription_type\":\"channel.chat.message\"},"
                          "\"payload\":{\"event\":{\"chatter_user_name\":\"ada\",\"message\":{\"text\":\"!ping\"}}}}";

static const char* CHAT_AGAIN = "{\"metadata\":{\"message_id\":\"n-1\",\"message_type\":\"notification\",\"subscription_type\":\"channel.chat.message\"},"
                                "\"payload\":{\"event\":{\"chatter_user_name\":\"ada\",\"message\":{\"text\":\"!ping\"}}}}";

static const char* RECONNECT = "{\"metadata\":{\"message_id\":\"r-1\",\"message_type\":\"session_reconnect\"},"
                               "\"payload\":{\"session\":{\"id\":\"session-abc\",\"reconnect_url\":\"wss://eventsub.wss.twitch.tv/ws?challenge=x\"}}}";

static const char* WELCOME_AGAIN = "{\"metadata\":{\"message_id\":\"w-2\",\"message_type\":\"session_welcome\"},"
                                   "\"payload\":{\"session\":{\"id\":\"session-def\",\"keepalive_timeout_seconds\":10}}}";

static const char* REVOCATION = "{\"metadata\":{\"message_id\":\"v-1\",\"message_type\":\"revocation\"},"
                                "\"payload\":{\"subscription\":{\"type\":\"channel.chat.message\",\"status\":\"authorization_revoked\"}}}";

static const char* STALE = "{\"metadata\":{\"message_id\":\"s-1\",\"message_type\":\"notification\","
                           "\"message_timestamp\":\"2020-01-01T00:00:00Z\",\"subscription_type\":\"channel.follow\"},"
                           "\"payload\":{\"event\":{}}}";

/*
 * ─────────────────────────────────────────────────────────
 * HELIX
 * ─────────────────────────────────────────────────────────
 */

typedef struct {
  u32         status;
  const char* headers;
  const char* body;
} Reply;

typedef struct {
  Reply script[SCRIPT_MAX];
  u32   script_count;
  u32   script_read;

  char last_url[512];
  char last_authorization[256];
  char last_client_id[128];
  char last_body[1024];
  u32  performed;

  u64 now_ms;
  u64 now_s;
} Helix;

static NYA_Error helix_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
  Helix* fake = (Helix*)user;

  (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", request.url);

  fake->last_authorization[0] = '\0';
  fake->last_client_id[0]     = '\0';

  for (u32 i = 0; i < NYA_REQUEST_MAX_HEADERS && request.headers[i].name != nullptr; i++) {
    if (nya_string_equals(request.headers[i].name, "Authorization")) {
      (void)snprintf(fake->last_authorization, sizeof(fake->last_authorization), "%s", request.headers[i].value);
    }
    if (nya_string_equals(request.headers[i].name, "Client-Id")) {
      (void)snprintf(fake->last_client_id, sizeof(fake->last_client_id), "%s", request.headers[i].value);
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

static u64 helix_now_ms(void* user) {
  return ((Helix*)user)->now_ms;
}

static u64 helix_now_s(void* user) {
  return ((Helix*)user)->now_s;
}

static void helix_push(Helix* fake, u32 status, const char* headers, const char* body) {
  nya_assert(fake->script_count < SCRIPT_MAX);

  fake->script[fake->script_count++] = (Reply){ .status = status, .headers = headers, .body = body };
}

static NYA_TwitchHelixOptions helix_options(Helix* fake) {
  return (NYA_TwitchHelixOptions){
    .token     = TEST_TOKEN,
    .client_id = "client-123",
    .bot_id    = "bot-456",
    .perform   = helix_perform,
    .now_ms    = helix_now_ms,
    .now_s     = helix_now_s,
    .user      = fake,
  };
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_twitch");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the welcome is the session, and a notification carries its event.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Sockets sockets = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchEventSub* events = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&sockets), &events).ok, "the client is made");

    NYA_TwitchEventSubMessage message = { 0 };

    nya_check(!nya_twitch_eventsub_poll(events, &message), "the first poll opens the socket and has nothing to say yet");
    nya_check(sockets.opens[0] == 1, "and the socket was opened, %u times", sockets.opens[0]);
    nya_check(nya_twitch_eventsub_state(events) == NYA_TWITCH_EVENTSUB_STATE_CONNECTING, "it is connecting, is '%s'",
              nya_twitch_eventsub_state_name(nya_twitch_eventsub_state(events)));

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_OPEN, nullptr);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, WELCOME);

    nya_check(nya_twitch_eventsub_poll(events, &message), "the welcome arrives");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_WELCOME, "as a welcome, got %u", (u32)message.kind);
    nya_check(nya_string_equals(message.session, "session-abc"), "with the session a subscription names, got '%s'", message.session);
    nya_check(nya_string_equals(nya_twitch_eventsub_session(events), "session-abc"), "which the client answers with too");
    nya_check(nya_twitch_eventsub_state(events) == NYA_TWITCH_EVENTSUB_STATE_READY, "and it is ready, is '%s'",
              nya_twitch_eventsub_state_name(nya_twitch_eventsub_state(events)));

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, CHAT);

    nya_check(nya_twitch_eventsub_poll(events, &message), "a chat message arrives");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_NOTIFICATION, "as a notification, got %u", (u32)message.kind);
    nya_check(nya_string_equals(message.subscription_type, "channel.chat.message"), "naming what it is, got '%s'", message.subscription_type);
    nya_check(message.event != nullptr, "and carrying the event itself");

    NYA_Value* chatter = message.event != nullptr ? nya_object_get(message.event, "chatter_user_name") : nullptr;
    nya_check(chatter != nullptr && nya_string_equals(chatter->as_string, "ada"), "which says who typed it");

    nya_twitch_eventsub_destroy(events);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a keepalive is not an event, and the same message twice is one event.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Sockets sockets = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchEventSub* events = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&sockets), &events).ok, "the client is made");

    NYA_TwitchEventSubMessage message = { 0 };
    (void)nya_twitch_eventsub_poll(events, &message);

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_OPEN, nullptr);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, WELCOME);
    nya_check(nya_twitch_eventsub_poll(events, &message), "the welcome arrives");

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, KEEPALIVE);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, CHAT);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, CHAT_AGAIN);

    nya_check(nya_twitch_eventsub_poll(events, &message), "the chat message comes through the keepalive");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_NOTIFICATION, "as the notification, got %u", (u32)message.kind);

    // twitch documents that a message may be delivered more than once, and the id is how that is seen.
    nya_check(!nya_twitch_eventsub_poll(events, &message), "the duplicate is dropped rather than handed over twice");

    nya_twitch_eventsub_destroy(events);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: silence past the keepalive interval ends the socket, with a backoff.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Sockets sockets = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchEventSub* events = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&sockets), &events).ok, "the client is made");

    NYA_TwitchEventSubMessage message = { 0 };
    (void)nya_twitch_eventsub_poll(events, &message);

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_OPEN, nullptr);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, WELCOME);
    nya_check(nya_twitch_eventsub_poll(events, &message), "the welcome arrives");

    // inside the promised interval plus its grace, an open socket saying nothing is a quiet channel.
    sockets.now_ms += 10000;
    nya_check(!nya_twitch_eventsub_poll(events, &message), "a quiet moment is not a failure");

    // past it, the socket is dead even though nothing said so, which is how this connection dies.
    sockets.now_ms += 10000;
    nya_check(nya_twitch_eventsub_poll(events, &message), "silence past the interval is reported");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_DISCONNECTED, "as a disconnection, got %u", (u32)message.kind);
    nya_check(message.retry_in_ms > 0, "with a wait before the next attempt, got " FMTu64, message.retry_in_ms);

    // kept, because the poll below writes the message it was read from.
    u64 retry_in_ms = message.retry_in_ms;
    nya_check(sockets.closes[0] == 1, "and the socket was closed, %u times", sockets.closes[0]);
    nya_check(nya_string_equals(nya_twitch_eventsub_session(events), ""), "the session is gone with it");

    // the backoff is a wait, not a stop: once it passes another socket is opened.
    nya_check(!nya_twitch_eventsub_poll(events, &message), "nothing happens inside the backoff");
    nya_check(sockets.opens[0] == 1, "and no socket was opened, %u so far", sockets.opens[0]);

    sockets.now_ms += retry_in_ms;
    (void)nya_twitch_eventsub_poll(events, &message);

    nya_check(sockets.opens[0] == 2, "once it passes another socket is opened, %u so far", sockets.opens[0]);

    nya_twitch_eventsub_destroy(events);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a reconnect moves to the new socket and loses nothing on the way.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Sockets sockets = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchEventSub* events = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&sockets), &events).ok, "the client is made");

    NYA_TwitchEventSubMessage message = { 0 };
    (void)nya_twitch_eventsub_poll(events, &message);

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_OPEN, nullptr);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, WELCOME);
    nya_check(nya_twitch_eventsub_poll(events, &message), "the welcome arrives");

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, RECONNECT);
    nya_check(!nya_twitch_eventsub_poll(events, &message), "the reconnect itself is not an event a caller sees");
    nya_check(sockets.opens[1] == 1, "a second socket was opened for it, %u", sockets.opens[1]);
    nya_check(nya_twitch_eventsub_state(events) == NYA_TWITCH_EVENTSUB_STATE_RECONNECTING, "and the client is moving, is '%s'",
              nya_twitch_eventsub_state_name(nya_twitch_eventsub_state(events)));

    // twitch keeps the old socket alive until the new one is welcomed, and so does this.
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, CHAT);
    nya_check(nya_twitch_eventsub_poll(events, &message) && message.kind == NYA_TWITCH_EVENTSUB_NOTIFICATION,
              "the old socket still delivers while the move is under way");

    frame_push(&sockets, 1, NYA_WEBSOCKET_EVENT_TEXT, WELCOME_AGAIN);
    nya_check(nya_twitch_eventsub_poll(events, &message), "the new socket is welcomed");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_WELCOME, "as a welcome, got %u", (u32)message.kind);

    // a new session has no subscriptions, which is why the caller is told rather than this being quiet.
    nya_check(nya_string_equals(message.session, "session-def"), "with a new session to subscribe against, got '%s'", message.session);
    nya_check(sockets.closes[0] == 1, "and the old socket was closed, %u times", sockets.closes[0]);

    nya_twitch_eventsub_destroy(events);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a revoked subscription, a stale message, and a url that will not open.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Sockets sockets = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchEventSub* events = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&sockets), &events).ok, "the client is made");

    NYA_TwitchEventSubMessage message = { 0 };
    (void)nya_twitch_eventsub_poll(events, &message);

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_OPEN, nullptr);
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, WELCOME);
    nya_check(nya_twitch_eventsub_poll(events, &message), "the welcome arrives");

    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, REVOCATION);
    nya_check(nya_twitch_eventsub_poll(events, &message), "a revocation is reported");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_REVOKED, "as a revocation, got %u", (u32)message.kind);
    nya_check(nya_string_equals(message.subscription_type, "channel.chat.message"), "naming what was lost, got '%s'", message.subscription_type);

    // older than twitch's ten minute replay window, so it is a capture rather than an event.
    frame_push(&sockets, 0, NYA_WEBSOCKET_EVENT_TEXT, STALE);
    nya_check(!nya_twitch_eventsub_poll(events, &message), "a message from before the replay window is dropped");

    nya_twitch_eventsub_destroy(events);

    Sockets refusing = { .now_ms = 1000, .now_s = 1'700'000'000, .refuse_open = true };

    NYA_TwitchEventSub* doomed = nullptr;
    nya_check(nya_twitch_eventsub_create(arena, socket_options(&refusing), &doomed).ok, "a client for a url that will not open is still made");

    nya_check(nya_twitch_eventsub_poll(doomed, &message), "the first poll reports the refusal");
    nya_check(message.kind == NYA_TWITCH_EVENTSUB_FATAL, "as fatal, got %u", (u32)message.kind);
    nya_check(!nya_twitch_eventsub_poll(doomed, &message), "and nothing happens after it");

    nya_twitch_eventsub_destroy(doomed);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a subscription names the session, the channel and the bot.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Helix fake = { .now_ms = 1000, .now_s = 1'700'000'000 };
    helix_push(&fake, 202, "ratelimit-limit: 800\r\nratelimit-remaining: 799\r\nratelimit-reset: 1700000060\r\n", "{\"data\":[{\"id\":\"sub-1\"}]}");

    NYA_TwitchHelix* helix = nullptr;
    nya_check(nya_twitch_helix_create(arena, helix_options(&fake), &helix).ok, "the client is made");

    u64 id = 0;
    nya_check(nya_twitch_helix_subscribe(helix, "channel.chat.message", "1", "channel-789", "session-abc", &id).ok, "the subscription is queued");

    NYA_TwitchHelixResult result = { 0 };
    nya_check(nya_twitch_helix_poll(helix, &result), "and goes out");
    nya_check(result.id == id && result.error.ok, "accepted: %s", (NYA_ConstCString)result.error.message);

    nya_check(nya_string_contains((NYA_ConstCString)fake.last_url, "/eventsub/subscriptions"), "to the subscription route, got '%s'", fake.last_url);
    nya_check(nya_string_equals(fake.last_authorization, "Bearer " TEST_TOKEN), "with the token as a bearer, got '%s'", fake.last_authorization);
    nya_check(nya_string_equals(fake.last_client_id, "client-123"), "and the client id beside it, got '%s'", fake.last_client_id);

    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "session-abc"), "the body names the session, got '%s'", fake.last_body);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "websocket"), "as a websocket transport");
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "channel-789"), "the channel it is about");
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "bot-456"), "and the account listening");

    NYA_TwitchHelixLimit limit = nya_twitch_helix_limit(helix);
    nya_check(limit.limit == 800 && limit.remaining == 799, "the bucket was read, %u of %u left", limit.remaining, limit.limit);

    // and the other half: a bot that stops caring about a channel says so, or the subscription keeps
    // counting against the limit twitch tracks per client id.
    helix_push(&fake, 204, "", "");

    nya_check(nya_twitch_helix_unsubscribe(helix, "sub-1", nullptr).ok, "unsubscribing is queued");
    nya_check(nya_twitch_helix_poll(helix, &result), "and goes out");
    nya_check(result.error.ok, "accepted: %s", (NYA_ConstCString)result.error.message);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_url, "id=sub-1"), "naming the subscription in the query, got '%s'", fake.last_url);
    nya_check(nya_string_equals(result.route, "/eventsub/subscriptions"), "while the route it reports carries no id, got '%s'", result.route);

    nya_check(!nya_twitch_helix_unsubscribe(helix, "", nullptr).ok, "and unsubscribing from nothing is refused");

    nya_twitch_helix_destroy(helix);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: chat goes over HTTP, and a spent bucket stops the client.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Helix fake = { .now_ms = 1000, .now_s = 1'700'000'000 };
    helix_push(&fake, 200, "ratelimit-limit: 800\r\nratelimit-remaining: 0\r\nratelimit-reset: 1700000060\r\n",
               "{\"data\":[{\"message_id\":\"m-1\",\"is_sent\":true}]}");
    helix_push(&fake, 200, "ratelimit-limit: 800\r\nratelimit-remaining: 799\r\nratelimit-reset: 1700000120\r\n",
               "{\"data\":[{\"message_id\":\"m-2\",\"is_sent\":true}]}");

    NYA_TwitchHelix* helix = nullptr;
    nya_check(nya_twitch_helix_create(arena, helix_options(&fake), &helix).ok, "the client is made");

    nya_check(nya_twitch_helix_chat_send(helix, "channel-789", "pong", nullptr).ok, "the message is queued");

    NYA_TwitchHelixResult result = { 0 };
    nya_check(nya_twitch_helix_poll(helix, &result), "and goes out");
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_url, "/chat/messages"), "over the chat route, got '%s'", fake.last_url);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "pong"), "carrying what to say, got '%s'", fake.last_body);

    // the reply said the bucket is empty until a reset a minute away, so nothing goes until then.
    nya_check(nya_twitch_helix_chat_send(helix, "channel-789", "again", nullptr).ok, "a second message is queued");
    nya_check(!nya_twitch_helix_poll(helix, &result), "but a spent bucket sends nothing");
    nya_check(fake.performed == 1, "so there was one transfer, made %u", fake.performed);

    fake.now_s = 1'700'000'061;

    nya_check(nya_twitch_helix_poll(helix, &result), "and once it refills the message goes");
    nya_check(result.error.ok, "accepted: %s", (NYA_ConstCString)result.error.message);

    nya_twitch_helix_destroy(helix);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the refusal a bot actually hits is said in the words that fix it.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Helix fake = { .now_ms = 1000, .now_s = 1'700'000'000 };
    helix_push(&fake, 401, "", "{\"error\":\"Unauthorized\",\"status\":401,\"message\":\"Invalid OAuth token\"}");

    NYA_TwitchHelix* helix = nullptr;
    nya_check(nya_twitch_helix_create(arena, helix_options(&fake), &helix).ok, "the client is made");
    nya_check(nya_twitch_helix_chat_send(helix, "channel-789", "hello", nullptr).ok, "the message is queued");

    NYA_TwitchHelixResult result = { 0 };
    nya_check(nya_twitch_helix_poll(helix, &result), "the refusal is reported");
    nya_check(!result.error.ok, "as a failure");
    nya_check(nya_string_contains((NYA_ConstCString)result.error.message, "user token"), "saying which token twitch wanted: %s",
              (NYA_ConstCString)result.error.message);

    // and the token is in no result, however it failed.
    nya_check(!nya_string_contains((NYA_ConstCString)result.error.message, TEST_TOKEN), "and never repeating the token itself");
    nya_check(!nya_string_contains((NYA_ConstCString)result.route, TEST_TOKEN), "not in the route either");

    nya_twitch_helix_destroy(helix);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what a client refuses before it ever sends anything.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    Helix fake = { .now_ms = 1000, .now_s = 1'700'000'000 };

    NYA_TwitchHelixOptions options = helix_options(&fake);
    options.token                  = nullptr;

    NYA_TwitchHelix* refused = nullptr;
    nya_check(!nya_twitch_helix_create(arena, options, &refused).ok, "a client with no token is refused");

    options           = helix_options(&fake);
    options.client_id = nullptr;
    nya_check(!nya_twitch_helix_create(arena, options, &refused).ok, "and so is one with no client id");

    options        = helix_options(&fake);
    options.bot_id = nullptr;

    NYA_TwitchHelix* anonymous = nullptr;
    nya_check(nya_twitch_helix_create(arena, options, &anonymous).ok, "a client with no bot id is made");

    // only the calls that need it are refused, and they are refused at the call rather than at the reply.
    nya_check(!nya_twitch_helix_chat_send(anonymous, "channel-789", "hello", nullptr).ok, "but it cannot send as anybody");
    nya_check(!nya_twitch_helix_subscribe(anonymous, "channel.chat.message", "1", "channel-789", "session-abc", nullptr).ok,
              "and cannot subscribe as anybody");

    NYA_TwitchHelix* helix = nullptr;
    nya_check(nya_twitch_helix_create(arena, helix_options(&fake), &helix).ok, "the client is made");

    nya_check(!nya_twitch_helix_subscribe(helix, "channel.chat.message", "1", "channel-789", "", nullptr).ok,
              "a subscription without a session is refused: it would deliver to nobody");
    nya_check(!nya_twitch_helix_chat_send(helix, "channel-789", "", nullptr).ok, "and a message with no text is not a message");
    nya_check(nya_twitch_helix_pending(helix) == 0, "nothing refused was queued, %u pending", nya_twitch_helix_pending(helix));

    nya_twitch_helix_destroy(helix);
    nya_twitch_helix_destroy(anonymous);
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
