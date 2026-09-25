/**
 * The Telegram bot client, driven through canned replies with no network: what the offset does, what
 * one update turns into, what a 429 costs, and that the token stays where it is put.
 *
 * The clock is a number this file moves, so a cooldown of a second is proved without waiting one.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Replies the fake holds before a test has to drain it. */
#define SCRIPT_MAX 8

/** Shaped like a real token and deliberately not one: digits, a colon, and a word saying what it is. */
#define TEST_TOKEN "000000000:this-is-a-test-fixture-and-not-a-token"

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
  char last_body[1024];
  u32  performed;

  u64 now_ms;
} Fake;

static NYA_Error fake_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
  Fake* fake = (Fake*)user;

  (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", request.url);

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

static NYA_TelegramOptions fake_options(Fake* fake) {
  return (NYA_TelegramOptions){
    .token   = TEST_TOKEN,
    .perform = fake_perform,
    .now_ms  = fake_now_ms,
    .user    = fake,
  };
}

/** One update as Telegram sends it, with the id and the text a case wants. */
static const char* UPDATES_ONE = "{\"ok\":true,\"result\":[{\"update_id\":42,\"message\":{\"message_id\":7,\"text\":\"/ping\","
                                 "\"chat\":{\"id\":-100123},\"from\":{\"id\":5150,\"first_name\":\"Ada\"}}}]}";

static const char* UPDATES_TWO = "{\"ok\":true,\"result\":[{\"update_id\":43,\"message\":{\"message_id\":8,\"text\":\"one\",\"chat\":{\"id\":1}}},"
                                 "{\"update_id\":44,\"message\":{\"message_id\":9,\"text\":\"two\",\"chat\":{\"id\":1}}}]}";

static const char* UPDATES_NONE = "{\"ok\":true,\"result\":[]}";

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_telegram");
  defer      nya_arena_destroy(arena);

  // TEST: a message becomes an update, and the offset moves past it.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", UPDATES_ONE);

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    NYA_TelegramUpdate update = { 0 };
    nya_check(nya_telegram_poll(bot, &update), "the poll hands over what arrived");

    nya_check(update.kind == NYA_TELEGRAM_UPDATE_MESSAGE, "it is a message, got %u", (u32)update.kind);
    nya_check(update.update_id == 42, "with telegram's id, got " FMTu64, update.update_id);
    nya_check(update.chat_id == -100123, "in the chat it was sent to, got " FMTs64, update.chat_id);
    nya_check(update.from_id == 5150 && nya_string_equals(update.from, "Ada"), "from who sent it, got '%s'", update.from);
    nya_check(nya_string_equals(update.text, "/ping"), "and says what they typed, got '%s'", update.text);

    // one past the highest handed over, which is what tells telegram it may forget it.
    nya_check(nya_telegram_offset(bot) == 43, "the offset moved to " FMTu64, nya_telegram_offset(bot));

    // the token is in the url telegram wants it in, and nowhere a result could carry it.
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_url, "/bot" TEST_TOKEN "/getUpdates"), "the poll went to the bot's own path");

    nya_telegram_destroy(bot);
  }

  // TEST: a batch is handed over one at a time, and one poll is one transfer.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", UPDATES_TWO);

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    NYA_TelegramUpdate first  = { 0 };
    NYA_TelegramUpdate second = { 0 };

    nya_check(nya_telegram_poll(bot, &first) && nya_string_equals(first.text, "one"), "the first one comes first, got '%s'", first.text);
    nya_check(nya_telegram_poll(bot, &second) && nya_string_equals(second.text, "two"), "then the second, got '%s'", second.text);

    nya_check(fake.performed == 1, "and both came out of one transfer, made %u", fake.performed);
    nya_check(nya_telegram_offset(bot) == 45, "the offset is past both, at " FMTu64, nya_telegram_offset(bot));

    // the interval is what stops a bot asking a hundred times a second for nothing.
    nya_check(!nya_telegram_poll(bot, &first), "a poll inside the interval asks for nothing");
    nya_check(fake.performed == 1, "so no second transfer happened, made %u", fake.performed);

    nya_telegram_destroy(bot);
  }

  // TEST: a send is queued, and goes out carrying the chat and the text.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", "{\"ok\":true,\"result\":{\"message_id\":11}}");

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    u64 id = 0;
    nya_check(nya_telegram_send(bot, -100123, "pong", &id).ok && id != 0, "the send is queued as " FMTu64, id);
    nya_check(nya_telegram_pending(bot) == 1, "and is waiting, %u pending", nya_telegram_pending(bot));

    NYA_TelegramResult result = { 0 };
    nya_check(nya_telegram_result_poll(bot, &result), "the poll sends it");

    nya_check(result.id == id && result.error.ok, "and reports it as sent: %s", (NYA_ConstCString)result.error.message);
    nya_check(nya_string_equals(result.method, "sendMessage"), "by the method it used, got '%s'", result.method);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "\"chat_id\""), "the body names the chat, got '%s'", fake.last_body);
    nya_check(nya_string_contains((NYA_ConstCString)fake.last_body, "pong"), "and carries the text, got '%s'", fake.last_body);
    nya_check(nya_telegram_pending(bot) == 0, "nothing is left queued, %u pending", nya_telegram_pending(bot));

    nya_telegram_destroy(bot);
  }

  // TEST: a 429 stops the client for as long as it was told, and the call is kept.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 429, "", "{\"ok\":false,\"error_code\":429,\"description\":\"Too Many Requests\",\"parameters\":{\"retry_after\":3}}");
    fake_push(&fake, 200, "", "{\"ok\":true,\"result\":{\"message_id\":12}}");

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    u64 id = 0;
    nya_check(nya_telegram_send(bot, 1, "hello", &id).ok, "the send is queued");

    NYA_TelegramResult result = { 0 };
    nya_check(!nya_telegram_result_poll(bot, &result), "the refused call is not reported as done");
    nya_check(nya_telegram_pending(bot) == 1, "it is still queued, %u pending", nya_telegram_pending(bot));

    nya_check(nya_telegram_cooldown_ms(bot, fake.now_ms) == 3000, "and nothing goes out for three seconds, got " FMTu64,
              nya_telegram_cooldown_ms(bot, fake.now_ms));

    nya_check(!nya_telegram_result_poll(bot, &result), "a poll inside the cooldown sends nothing");
    nya_check(fake.performed == 1, "so the transfer count is still one, made %u", fake.performed);

    // past the cooldown and past the backoff, the same message goes rather than a different one.
    fake.now_ms += 4000;

    nya_check(nya_telegram_result_poll(bot, &result), "once it passes the call goes");
    nya_check(result.id == id && result.error.ok, "and it is the one that was refused: %s", (NYA_ConstCString)result.error.message);

    nya_telegram_destroy(bot);
  }

  // TEST: telegram refusing on its own terms is a failure, whatever the status says.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", "{\"ok\":false,\"description\":\"chat not found\"}");

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");
    nya_check(nya_telegram_send(bot, 9, "into the void", nullptr).ok, "the send is queued");

    NYA_TelegramResult result = { 0 };
    nya_check(nya_telegram_result_poll(bot, &result), "the call is reported");
    nya_check(!result.error.ok, "as the failure it is");
    nya_check(nya_string_contains((NYA_ConstCString)result.error.message, "chat not found"), "saying what telegram said: %s",
              (NYA_ConstCString)result.error.message);

    nya_telegram_destroy(bot);
  }

  // TEST: a pressed button, and the answer it is waiting for.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "",
              "{\"ok\":true,\"result\":[{\"update_id\":90,\"callback_query\":{\"id\":\"cb-1\",\"data\":\"yes\","
              "\"from\":{\"id\":7,\"first_name\":\"Grace\"},\"message\":{\"message_id\":3,\"chat\":{\"id\":55}}}}]}");
    fake_push(&fake, 200, "", "{\"ok\":true,\"result\":true}");

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    NYA_TelegramUpdate update = { 0 };
    nya_check(nya_telegram_poll(bot, &update), "the press arrives");
    nya_check(update.kind == NYA_TELEGRAM_UPDATE_CALLBACK_QUERY, "as a callback query, got %u", (u32)update.kind);
    nya_check(nya_string_equals(update.callback_id, "cb-1"), "with the id that answers it, got '%s'", update.callback_id);
    nya_check(nya_string_equals(update.text, "yes"), "and the data the button carried, got '%s'", update.text);
    nya_check(update.chat_id == 55 && update.message_id == 3, "in the chat the button is in, got " FMTs64, update.chat_id);

    nya_check(nya_telegram_answer_callback(bot, update.callback_id, "done", nullptr).ok, "the answer is queued");

    NYA_TelegramResult result = { 0 };
    nya_check(nya_telegram_result_poll(bot, &result), "and goes out");
    nya_check(nya_string_equals(result.method, "answerCallbackQuery"), "by its own method, got '%s'", result.method);

    nya_telegram_destroy(bot);
  }

  // TEST: an update this does not model still moves the offset.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", "{\"ok\":true,\"result\":[{\"update_id\":77,\"poll\":{\"id\":\"p\"}}]}");

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    NYA_TelegramUpdate update = { 0 };
    nya_check(nya_telegram_poll(bot, &update), "it is handed over rather than dropped");
    nya_check(update.kind == NYA_TELEGRAM_UPDATE_OTHER, "as something this does not model, got %u", (u32)update.kind);
    nya_check(nya_telegram_offset(bot) == 78, "and the offset passed it, at " FMTu64, nya_telegram_offset(bot));

    nya_telegram_destroy(bot);
  }

  // TEST: an empty answer is no update, and the create refuses what cannot work.
  {
    Fake fake = { .now_ms = 1000 };
    fake_push(&fake, 200, "", UPDATES_NONE);

    NYA_Telegram* bot = nullptr;
    nya_check(nya_telegram_create(arena, fake_options(&fake), &bot).ok, "the client is made");

    NYA_TelegramUpdate update = { 0 };
    nya_check(!nya_telegram_poll(bot, &update), "a quiet chat is no update");
    nya_check(nya_telegram_offset(bot) == 0, "and the offset stayed at " FMTu64, nya_telegram_offset(bot));

    nya_telegram_destroy(bot);

    NYA_TelegramOptions bad = fake_options(&fake);
    bad.token               = nullptr;

    NYA_Telegram* refused = nullptr;
    nya_check(!nya_telegram_create(arena, bad, &refused).ok, "a client with no token is refused");

    // a poll the transfer gives up on before the server answers would look exactly like a quiet chat.
    bad                = fake_options(&fake);
    bad.timeout_ms     = 2000;
    bad.poll_timeout_s = 30;

    nya_check(!nya_telegram_create(arena, bad, &refused).ok, "and so is a long poll the transfer cannot outlive");
  }

  // TEST: the webhook secret, which is the only proof telegram offers.
  {
    NYA_HttpRequest request = { 0 };
    // lower case, because that is what the parser stores: nya_http_request_header lowers the name it is asked for and compares it against bytes that are already lowered.
    (void)snprintf(request.headers[0].name, sizeof(request.headers[0].name), "%s", "x-telegram-bot-api-secret-token");
    (void)snprintf(request.headers[0].value, sizeof(request.headers[0].value), "%s", "a-secret-nobody-else-knows");
    request.header_count = 1;

    NYA_HttpExchange exchange = { .request = &request };

    nya_check(nya_telegram_webhook_verify(&exchange, "a-secret-nobody-else-knows"), "the secret this bot registered is accepted");
    nya_check(!nya_telegram_webhook_verify(&exchange, "a-secret-somebody-guessed"), "one letter off is refused");
    nya_check(!nya_telegram_webhook_verify(&exchange, "a-secret-nobody-else-knows-and-then-some"), "and so is a longer one that starts the same");
    nya_check(!nya_telegram_webhook_verify(&exchange, ""), "a bot with no secret verifies nothing");

    NYA_HttpRequest bare      = { 0 };
    NYA_HttpExchange no_header = { .request = &bare };

    nya_check(!nya_telegram_webhook_verify(&no_header, "a-secret-nobody-else-knows"), "a request without the header proves nothing");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
