#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-plugins/telegram_bot/telegram.h"

// ───────────────────────────────────── PRIVATE TYPES ─────────────────────────────────────

/** One queued call, held until it has been sent or has run out of attempts. */
typedef struct {
    u64                  id;
    NYA_TelegramCallKind kind;

    s64  chat_id;
    char callback_id[NYA_TELEGRAM_MAX_ID];
    char text[NYA_TELEGRAM_MAX_TEXT];

    u32 attempts;
    u64 ready_at_ms;
} _NYA_TelegramCall;

struct NYA_Telegram {
    NYA_Arena* arena;

    /** Emptied at the top of every transfer: a reply's body lives exactly until the next one. */
    NYA_Arena* exchanges;

    char token[NYA_TELEGRAM_MAX_TOKEN];
    char base_url[256];

    /** Built per transfer and wiped after it, because the token is in it. */
    char url[512];

    u64 timeout_ms;
    u32 poll_timeout_s;

    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    void* user;

    /** What the next getUpdates asks from, and when the last one was made. */
    u64 offset;
    u64 polled_at_ms;

    /** Nothing is sent before this. What a 429 left behind; see the header's rate limit note. */
    u64 cooldown_until_ms;

    NYA_TelegramUpdate updates[NYA_TELEGRAM_MAX_UPDATES];
    u32                update_count;
    u32                update_read;

    _NYA_TelegramCall queue[NYA_TELEGRAM_MAX_QUEUE];
    u32               queue_count;

    /** Counts up forever, so an id names one call for the life of the process. */
    u64 next_id;
};

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** The default transport: nya_request_perform, and the monotonic clock. */
NYA_INTERNAL NYA_Error _nya_telegram_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;
NYA_INTERNAL u64       _nya_telegram_now_ms(void* user) __attr_no_discard;

/** Copies `text` into `destination`, truncating rather than running over. */
NYA_INTERNAL void _nya_telegram_copy(char* destination, u64 capacity, NYA_ConstCString text);

/** A string field of a JSON object, or null where it is absent or another type. */
NYA_INTERNAL NYA_ConstCString _nya_telegram_string_at(const NYA_Object* object, NYA_CString key) __attr_no_discard;

/** A number field of a JSON object, or `fallback`. Both integer and real, since json is json. */
NYA_INTERNAL s64 _nya_telegram_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) __attr_no_discard;

/** An object field of a JSON object, or null. */
NYA_INTERNAL const NYA_Object* _nya_telegram_object_at(const NYA_Object* object, NYA_CString key) __attr_no_discard;

/** Queues one call, or refuses it because the queue is full. */
NYA_INTERNAL NYA_Error _nya_telegram_queue(NYA_Telegram* bot, _NYA_TelegramCall call, OUT u64* out_id) __attr_no_discard;

/** Drops the entry at `index`, keeping the order of the rest: a bot's messages are a conversation. */
NYA_INTERNAL void _nya_telegram_dequeue(NYA_Telegram* bot, u32 index);

/** Performs one call, filling `out_url` with the method's url. The token is in it, so it is wiped after. */
NYA_INTERNAL NYA_Error _nya_telegram_call(NYA_Telegram* bot, NYA_ConstCString method, NYA_Object* body, OUT NYA_Response* out_response) __attr_no_discard;

/** The seconds a 429 asked for, from the body Telegram answers with or the header it sometimes adds. */
NYA_INTERNAL u64 _nya_telegram_retry_after_ms(const NYA_Response* response) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_telegram_create(NYA_Arena* arena, NYA_TelegramOptions options, NYA_Telegram** out_bot) {
    nya_assert(arena != nullptr && out_bot != nullptr);

    *out_bot = nullptr;

    if (options.token == nullptr || options.token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a telegram bot needs a token");
    if (strlen(options.token) >= NYA_TELEGRAM_MAX_TOKEN) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token is longer than this client holds");

    u64 timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : NYA_REQUEST_DEFAULT_TIMEOUT_MS;

    // A poll the transfer gives up on before the server answers would look exactly like a quiet chat, so the pair is refused here rather than puzzled over later.
    if ((u64)options.poll_timeout_s * 1000 >= timeout_ms) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "poll_timeout_s must leave the transfer time to answer; raise timeout_ms");
    }

    NYA_Telegram* bot = nya_arena_alloc(arena, sizeof(NYA_Telegram));
    nya_memset(bot, 0, sizeof(NYA_Telegram));

    bot->arena     = arena;
    bot->exchanges = nya_arena_create(.name = "telegram_exchanges");

    _nya_telegram_copy(bot->token, sizeof(bot->token), options.token);
    _nya_telegram_copy(bot->base_url, sizeof(bot->base_url), options.base_url != nullptr ? options.base_url : NYA_TELEGRAM_URL);

    bot->timeout_ms     = timeout_ms;
    bot->poll_timeout_s = options.poll_timeout_s;

    bot->perform = options.perform != nullptr ? options.perform : _nya_telegram_perform;
    bot->now_ms  = options.now_ms != nullptr ? options.now_ms : _nya_telegram_now_ms;
    bot->user    = options.user;

    bot->next_id = 1;

    *out_bot = bot;

    return NYA_OK;
}

void nya_telegram_destroy(NYA_Telegram* bot) {
    if (bot == nullptr) return;

    // The token first, so it is gone even if the arena's memory is handed straight back out.
    nya_crypto_wipe(bot->token, sizeof(bot->token));
    nya_crypto_wipe(bot->url, sizeof(bot->url));

    nya_arena_destroy(bot->exchanges);
}

b8 nya_telegram_poll(NYA_Telegram* bot, NYA_TelegramUpdate* out_update) {
    nya_assert(bot != nullptr && out_update != nullptr);

    // What came back last time, one at a time; the offset moves as each is handed over, so an unseen update is one Telegram must keep. See the header.
    if (bot->update_read < bot->update_count) {
        *out_update = bot->updates[bot->update_read];

        bot->update_read += 1;
        bot->offset       = out_update->update_id + 1;

        return true;
    }

    u64 now_ms = bot->now_ms(bot->user);

    if (bot->polled_at_ms != 0 && now_ms - bot->polled_at_ms < NYA_TELEGRAM_POLL_INTERVAL_MS && bot->poll_timeout_s == 0) return false;

    bot->polled_at_ms = now_ms;
    bot->update_count = 0;
    bot->update_read  = 0;

    NYA_Object* body = nya_object_create(bot->exchanges);
    nya_object_add(body, "offset", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)bot->offset });
    nya_object_add(body, "limit", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = NYA_TELEGRAM_MAX_UPDATES });
    nya_object_add(body, "timeout", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = bot->poll_timeout_s });

    NYA_Response response  = { 0 };
    NYA_Error    performed = _nya_telegram_call(bot, "getUpdates", body, &response);

    if (!performed.ok) {
        nya_log_warn("The telegram poll failed: %s", (NYA_ConstCString)performed.message);
        return false;
    }

    if (response.status == 429) {
        bot->cooldown_until_ms = now_ms + _nya_telegram_retry_after_ms(&response);
        return false;
    }

    if (response.status != 200 || response.body == nullptr) return false;

    NYA_Value* result = nya_object_get(response.body, "result");
    if (result == nullptr || result->type != NYA_TYPE_ARRAY) return false;

    nya_array_foreach (&result->as_array, entry) {
        if (bot->update_count >= NYA_TELEGRAM_MAX_UPDATES) break;
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_TelegramUpdate update = { 0 };
        if (!nya_telegram_update_read(&entry->as_object, &update)) continue;

        bot->updates[bot->update_count] = update;
        bot->update_count              += 1;
    }

    if (bot->update_count == 0) return false;

    *out_update = bot->updates[0];

    bot->update_read = 1;
    bot->offset      = out_update->update_id + 1;

    return true;
}

NYA_Error nya_telegram_send(NYA_Telegram* bot, s64 chat_id, NYA_ConstCString text, u64* out_id) {
    nya_assert(bot != nullptr);

    if (text == nullptr || text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message with no text is not a message");
    if (strlen(text) >= NYA_TELEGRAM_MAX_TEXT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the message is longer than telegram accepts");

    _NYA_TelegramCall call = { .kind = NYA_TELEGRAM_CALL_SEND_MESSAGE, .chat_id = chat_id };
    _nya_telegram_copy(call.text, sizeof(call.text), text);

    return _nya_telegram_queue(bot, call, out_id);
}

NYA_Error nya_telegram_answer_callback(NYA_Telegram* bot, NYA_ConstCString callback_id, NYA_ConstCString text, u64* out_id) {
    nya_assert(bot != nullptr);

    if (callback_id == nullptr || callback_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "answering a callback needs its id");
    if (strlen(callback_id) >= NYA_TELEGRAM_MAX_ID) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a callback id this client can hold");

    _NYA_TelegramCall call = { .kind = NYA_TELEGRAM_CALL_ANSWER_CALLBACK };
    _nya_telegram_copy(call.callback_id, sizeof(call.callback_id), callback_id);
    _nya_telegram_copy(call.text, sizeof(call.text), text != nullptr ? text : "");

    return _nya_telegram_queue(bot, call, out_id);
}

b8 nya_telegram_result_poll(NYA_Telegram* bot, NYA_TelegramResult* out_result) {
    nya_assert(bot != nullptr && out_result != nullptr);

    if (bot->queue_count == 0) return false;

    u64 now_ms = bot->now_ms(bot->user);

    if (now_ms < bot->cooldown_until_ms) return false;

    // The front one only: a bot's calls are a conversation, and answering the second first because the first is backing off reads as broken.
    _NYA_TelegramCall call = bot->queue[0];

    if (now_ms < call.ready_at_ms) return false;

    NYA_ConstCString method = call.kind == NYA_TELEGRAM_CALL_SEND_MESSAGE ? "sendMessage" : "answerCallbackQuery";

    NYA_Object* body = nya_object_create(bot->exchanges);

    switch (call.kind) {
        case NYA_TELEGRAM_CALL_SEND_MESSAGE: {
            nya_object_add(body, "chat_id", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = call.chat_id });
            nya_object_add(body, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = call.text });
            break;
        }

        case NYA_TELEGRAM_CALL_ANSWER_CALLBACK:
        case NYA_TELEGRAM_CALL_KIND_COUNT:
        default: {
            nya_object_add(body, "callback_query_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = call.callback_id });
            if (call.text[0] != '\0') nya_object_add(body, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = call.text });
            break;
        }
    }

    NYA_Response response  = { 0 };
    NYA_Error    performed = _nya_telegram_call(bot, method, body, &response);

    if (response.status == 429) {
        bot->cooldown_until_ms = now_ms + _nya_telegram_retry_after_ms(&response);
    }

    b8 retryable = response.status == 429 || (response.status >= 500 && response.status < 600) || !performed.ok;

    if (retryable && call.attempts + 1 < NYA_TELEGRAM_MAX_ATTEMPTS) {
        bot->queue[0].attempts    += 1;
        bot->queue[0].ready_at_ms  = now_ms + ((u64)NYA_TELEGRAM_RETRY_MS << call.attempts);

        return false;
    }

    _nya_telegram_dequeue(bot, 0);

    *out_result = (NYA_TelegramResult){
        .id     = call.id,
        .kind   = call.kind,
        .method = method,
        .status = response.status,
        .error  = performed,
    };

    // A call Telegram refused on its own terms, which is a 200 with `ok: false` as often as it is a 400.
    if (performed.ok && (response.status != 200 || (response.body != nullptr && _nya_telegram_integer_at(response.body, "ok", 1) == 0))) {
        NYA_ConstCString description = response.body != nullptr ? _nya_telegram_string_at(response.body, "description") : nullptr;

        out_result->error = nya_error(NYA_ERROR_NOT_OK, "telegram refused %s: %s", method, description != nullptr ? description : "no reason given");
    }

    return true;
}

u32 nya_telegram_pending(const NYA_Telegram* bot) {
    nya_assert(bot != nullptr);

    return bot->queue_count;
}

u64 nya_telegram_offset(const NYA_Telegram* bot) {
    nya_assert(bot != nullptr);

    return bot->offset;
}

u64 nya_telegram_cooldown_ms(const NYA_Telegram* bot, u64 now_ms) {
    nya_assert(bot != nullptr);

    return now_ms < bot->cooldown_until_ms ? bot->cooldown_until_ms - now_ms : 0;
}

b8 nya_telegram_webhook_verify(const NYA_HttpExchange* exchange, NYA_ConstCString secret) {
    nya_assert(exchange != nullptr && exchange->request != nullptr);

    if (secret == nullptr || secret[0] == '\0') return false;

    NYA_ConstCString sent = nya_http_request_header(exchange->request, NYA_TELEGRAM_SECRET_HEADER);
    if (sent == nullptr) return false;

    u64 expected = strlen(secret);

    // Length first, then the bytes in constant time: a different-length secret is not this bot's, and its length was never the part worth hiding.
    if (strlen(sent) != expected) return false;

    return nya_crypto_equals((const u8*)sent, (const u8*)secret, expected);
}

b8 nya_telegram_update_read(const NYA_Object* object, NYA_TelegramUpdate* out_update) {
    nya_assert(out_update != nullptr);

    nya_memset(out_update, 0, sizeof(NYA_TelegramUpdate));

    if (object == nullptr) return false;

    s64 id = _nya_telegram_integer_at(object, "update_id", -1);
    if (id < 0) return false;

    out_update->update_id = (u64)id;

    const NYA_Object* message = _nya_telegram_object_at(object, "message");
    const NYA_Object* edited  = _nya_telegram_object_at(object, "edited_message");
    const NYA_Object* query   = _nya_telegram_object_at(object, "callback_query");

    if (query != nullptr) {
        out_update->kind = NYA_TELEGRAM_UPDATE_CALLBACK_QUERY;

        _nya_telegram_copy(out_update->callback_id, sizeof(out_update->callback_id), _nya_telegram_string_at(query, "id"));

        // The data the button carried, which is what a bot switches on; Telegram names the visible label separately and never sends it back.
        _nya_telegram_copy(out_update->text, sizeof(out_update->text), _nya_telegram_string_at(query, "data"));

        const NYA_Object* from = _nya_telegram_object_at(query, "from");
        if (from != nullptr) {
            out_update->from_id = (u64)_nya_telegram_integer_at(from, "id", 0);
            _nya_telegram_copy(out_update->from, sizeof(out_update->from), _nya_telegram_string_at(from, "first_name"));
        }

        // A button lives under a message, and answering in the chat it was pressed in needs that message.
        const NYA_Object* under = _nya_telegram_object_at(query, "message");
        if (under != nullptr) {
            out_update->message_id = (u64)_nya_telegram_integer_at(under, "message_id", 0);

            const NYA_Object* chat = _nya_telegram_object_at(under, "chat");
            if (chat != nullptr) out_update->chat_id = _nya_telegram_integer_at(chat, "id", 0);
        }

        return true;
    }

    const NYA_Object* carried = message != nullptr ? message : edited;

    if (carried == nullptr) {
        // Modelled as far as its id, which is all the offset needs; a poll or chat member change is still resent until acknowledged.
        out_update->kind = NYA_TELEGRAM_UPDATE_OTHER;
        return true;
    }

    out_update->kind       = message != nullptr ? NYA_TELEGRAM_UPDATE_MESSAGE : NYA_TELEGRAM_UPDATE_EDITED_MESSAGE;
    out_update->message_id = (u64)_nya_telegram_integer_at(carried, "message_id", 0);

    _nya_telegram_copy(out_update->text, sizeof(out_update->text), _nya_telegram_string_at(carried, "text"));

    const NYA_Object* chat = _nya_telegram_object_at(carried, "chat");
    if (chat != nullptr) out_update->chat_id = _nya_telegram_integer_at(chat, "id", 0);

    const NYA_Object* from = _nya_telegram_object_at(carried, "from");
    if (from != nullptr) {
        out_update->from_id = (u64)_nya_telegram_integer_at(from, "id", 0);
        _nya_telegram_copy(out_update->from, sizeof(out_update->from), _nya_telegram_string_at(from, "first_name"));
    }

    return true;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_telegram_perform(void* user, NYA_Arena* arena, NYA_Request request, NYA_Response* out_response) {
    (void)user;

    return nya_request_perform(arena, request, out_response);
}

u64 _nya_telegram_now_ms(void* user) {
    (void)user;

    return nya_clock_get_monotonic_ms();
}

void _nya_telegram_copy(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(destination != nullptr && capacity > 0);

    destination[0] = '\0';

    if (text == nullptr) return;

    u64 length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(destination, text, length);
    destination[length] = '\0';
}

NYA_ConstCString _nya_telegram_string_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

s64 _nya_telegram_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) {
    if (object == nullptr) return fallback;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr) return fallback;

    // Chat ids arrive as integers and `retry_after` as either, so both are numbers here.
    if (value->type == NYA_TYPE_S64) return value->as_s64;
    if (value->type == NYA_TYPE_F64) return (s64)value->as_f64;
    if (value->type == NYA_TYPE_B8) return value->as_b8 ? 1 : 0;

    return fallback;
}

const NYA_Object* _nya_telegram_object_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_OBJECT) return nullptr;

    return &value->as_object;
}

NYA_Error _nya_telegram_queue(NYA_Telegram* bot, _NYA_TelegramCall call, u64* out_id) {
    if (bot->queue_count >= NYA_TELEGRAM_MAX_QUEUE) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the telegram queue is full at %d calls", NYA_TELEGRAM_MAX_QUEUE);
    }

    call.id = bot->next_id;

    bot->next_id                 += 1;
    bot->queue[bot->queue_count]  = call;
    bot->queue_count             += 1;

    if (out_id != nullptr) *out_id = call.id;

    return NYA_OK;
}

void _nya_telegram_dequeue(NYA_Telegram* bot, u32 index) {
    nya_assert(index < bot->queue_count);

    for (u32 i = index; i + 1 < bot->queue_count; i++) bot->queue[i] = bot->queue[i + 1];

    bot->queue_count                 -= 1;
    bot->queue[bot->queue_count]      = (_NYA_TelegramCall){ 0 };
}

NYA_Error _nya_telegram_call(NYA_Telegram* bot, NYA_ConstCString method, NYA_Object* body, NYA_Response* out_response) {
    // Last reply's body, and the object just built, both go here: an exchange's memory lives exactly
    // as long as the exchange after it takes to start.
    (void)snprintf(bot->url, sizeof(bot->url), "%s/bot%s/%s", bot->base_url, bot->token, method);

    NYA_Error performed = bot->perform(bot->user, bot->exchanges,
                                       (NYA_Request){
                                           .method     = NYA_REQUEST_METHOD_POST,
                                           .url        = bot->url,
                                           .body       = body,
                                           .timeout_ms = bot->timeout_ms,
                                       },
                                       out_response);

    // The token was in that url and nowhere else, so it stops existing the moment the transfer is over.
    nya_crypto_wipe(bot->url, sizeof(bot->url));

    return performed;
}

u64 _nya_telegram_retry_after_ms(const NYA_Response* response) {
    s64 seconds = 0;

    // Where Telegram actually puts it: inside `parameters`, beside whatever else it wants to say.
    if (response->body != nullptr) {
        const NYA_Object* parameters = _nya_telegram_object_at(response->body, "parameters");
        seconds                      = _nya_telegram_integer_at(parameters, "retry_after", 0);
    }

    // The header is what a proxy in front of it adds, and is the only one some deployments send.
    if (seconds <= 0) {
        char header[32] = { 0 };

        if (nya_response_header(response, "Retry-After", header, sizeof(header))) {
            u64 parsed = 0;
            if (nya_type_parse(NYA_TYPE_U64, (const u8*)header, strlen(header), &parsed)) seconds = (s64)parsed;
        }
    }

    // A 429 with nothing to say still means stop for a moment, so the floor is a second rather than none.
    if (seconds <= 0) seconds = 1;

    return (u64)seconds * 1000;
}
