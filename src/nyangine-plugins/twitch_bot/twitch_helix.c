#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-plugins/twitch_bot/twitch_helix.h"

// ───────────────────────────────────── PRIVATE TYPES ─────────────────────────────────────

/** One queued call, held until it has been sent or has run out of attempts. */
typedef struct {
    u64                     id;
    NYA_TwitchHelixCallKind kind;

    char type[NYA_TWITCH_HELIX_MAX_TYPE];
    char version[NYA_TWITCH_HELIX_MAX_ID];
    char broadcaster_id[NYA_TWITCH_HELIX_MAX_ID];
    char session[NYA_TWITCH_HELIX_MAX_SESSION];
    char subscription_id[NYA_TWITCH_HELIX_MAX_ID];
    char text[NYA_TWITCH_HELIX_MAX_TEXT];

    u32 attempts;
    u64 ready_at_ms;
} _NYA_TwitchHelixCall;

struct NYA_TwitchHelix {
    NYA_Arena* arena;

    /** Emptied at the top of every transfer: a reply's body lives exactly until the next one. */
    NYA_Arena* exchanges;

    char token[NYA_TWITCH_HELIX_MAX_TOKEN];
    char client_id[NYA_TWITCH_HELIX_MAX_ID];
    char bot_id[NYA_TWITCH_HELIX_MAX_ID];
    char base_url[256];

    /** Built per transfer and wiped after it, because the token is in the header it holds. */
    char url[512];
    char authorization[NYA_TWITCH_HELIX_MAX_TOKEN + 16];

    /** The route of the call last reported, so a result points at the client rather than at scratch. */
    char last_route[NYA_TWITCH_HELIX_MAX_ROUTE];

    u64 timeout_ms;

    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    u64 (*now_s)(void* user);
    void* user;

    NYA_TwitchHelixLimit limit;

    _NYA_TwitchHelixCall queue[NYA_TWITCH_HELIX_MAX_QUEUE];
    u32                  queue_count;

    /** Counts up forever, so an id names one call for the life of the process. */
    u64 next_id;
};

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/* The default transport: nya_request_perform, and the engine's clocks. */
NYA_INTERNAL NYA_Error _nya_twitch_helix_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;
NYA_INTERNAL u64       _nya_twitch_helix_now_ms(void* user) __attr_no_discard;
NYA_INTERNAL u64       _nya_twitch_helix_now_s(void* user) __attr_no_discard;

/** Copies `text` into `destination`, truncating rather than running over. */
NYA_INTERNAL void _nya_twitch_helix_copy(char* destination, u64 capacity, NYA_ConstCString text);

/** Queues one call, or refuses it because the queue is full. */
NYA_INTERNAL NYA_Error _nya_twitch_helix_queue(NYA_TwitchHelix* helix, _NYA_TwitchHelixCall call, OUT u64* out_id) __attr_no_discard;

/** Drops the entry at `index`, keeping the order of the rest: a bot's calls are a conversation. */
NYA_INTERNAL void _nya_twitch_helix_dequeue(NYA_TwitchHelix* helix, u32 index);

/** The body one call sends. */
NYA_INTERNAL NYA_Object* _nya_twitch_helix_body(NYA_TwitchHelix* helix, const _NYA_TwitchHelixCall* call) __attr_no_discard;

/** Folds one reply's rate limit headers into the client's bucket. */
NYA_INTERNAL void _nya_twitch_helix_observe(NYA_TwitchHelix* helix, const NYA_Response* response);

/** A u64 header, or `fallback` where it is absent or not a number. */
NYA_INTERNAL u64 _nya_twitch_helix_header_u64(const NYA_Response* response, NYA_ConstCString name, u64 fallback) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_twitch_helix_create(NYA_Arena* arena, NYA_TwitchHelixOptions options, NYA_TwitchHelix** out_helix) {
    nya_assert(arena != nullptr && out_helix != nullptr);

    *out_helix = nullptr;

    if (options.token == nullptr || options.token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a helix client needs a token");
    if (strlen(options.token) >= NYA_TWITCH_HELIX_MAX_TOKEN) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token is longer than this client holds");

    // Twitch answers a missing client id with a 401 that says nothing about which half is missing, so it is required here rather than discovered from a refusal.
    if (options.client_id == nullptr || options.client_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a helix client needs a client id");
    if (strlen(options.client_id) >= NYA_TWITCH_HELIX_MAX_ID) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a client id");

    NYA_TwitchHelix* helix = nya_arena_alloc(arena, sizeof(NYA_TwitchHelix));
    nya_memset(helix, 0, sizeof(NYA_TwitchHelix));

    helix->arena     = arena;
    helix->exchanges = nya_arena_create(.name = "twitch_helix_exchanges");

    _nya_twitch_helix_copy(helix->token, sizeof(helix->token), options.token);
    _nya_twitch_helix_copy(helix->client_id, sizeof(helix->client_id), options.client_id);
    _nya_twitch_helix_copy(helix->bot_id, sizeof(helix->bot_id), options.bot_id != nullptr ? options.bot_id : "");
    _nya_twitch_helix_copy(helix->base_url, sizeof(helix->base_url), options.base_url != nullptr ? options.base_url : NYA_TWITCH_HELIX_URL);

    helix->timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : NYA_REQUEST_DEFAULT_TIMEOUT_MS;

    helix->perform = options.perform != nullptr ? options.perform : _nya_twitch_helix_perform;
    helix->now_ms  = options.now_ms != nullptr ? options.now_ms : _nya_twitch_helix_now_ms;
    helix->now_s   = options.now_s != nullptr ? options.now_s : _nya_twitch_helix_now_s;
    helix->user    = options.user;

    helix->next_id = 1;

    *out_helix = helix;

    return NYA_OK;
}

void nya_twitch_helix_destroy(NYA_TwitchHelix* helix) {
    if (helix == nullptr) return;

    nya_crypto_wipe(helix->token, sizeof(helix->token));
    nya_crypto_wipe(helix->authorization, sizeof(helix->authorization));

    nya_arena_destroy(helix->exchanges);
}

NYA_Error nya_twitch_helix_subscribe(NYA_TwitchHelix* helix, NYA_ConstCString type, NYA_ConstCString version, NYA_ConstCString broadcaster_id,
                                     NYA_ConstCString session, u64* out_id) {
    nya_assert(helix != nullptr);

    if (type == nullptr || type[0] == '\0' || strlen(type) >= NYA_TWITCH_HELIX_MAX_TYPE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a subscription needs a type, such as channel.chat.message");
    }

    if (broadcaster_id == nullptr || broadcaster_id[0] == '\0' || strlen(broadcaster_id) >= NYA_TWITCH_HELIX_MAX_ID) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a subscription needs the channel it is about");
    }

    // A subscription belongs to one session, and an unknown session id would be accepted and then deliver to nobody.
    if (session == nullptr || session[0] == '\0' || strlen(session) >= NYA_TWITCH_HELIX_MAX_SESSION) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a subscription needs the session it should arrive on");
    }

    if (helix->bot_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a subscription is made as the bot, so bot_id is needed");

    _NYA_TwitchHelixCall call = { .kind = NYA_TWITCH_HELIX_CALL_SUBSCRIBE };

    _nya_twitch_helix_copy(call.type, sizeof(call.type), type);
    _nya_twitch_helix_copy(call.version, sizeof(call.version), version != nullptr && version[0] != '\0' ? version : "1");
    _nya_twitch_helix_copy(call.broadcaster_id, sizeof(call.broadcaster_id), broadcaster_id);
    _nya_twitch_helix_copy(call.session, sizeof(call.session), session);

    return _nya_twitch_helix_queue(helix, call, out_id);
}

NYA_Error nya_twitch_helix_unsubscribe(NYA_TwitchHelix* helix, NYA_ConstCString subscription_id, u64* out_id) {
    nya_assert(helix != nullptr);

    if (subscription_id == nullptr || subscription_id[0] == '\0' || strlen(subscription_id) >= NYA_TWITCH_HELIX_MAX_ID) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unsubscribing needs the id the subscription was accepted under");
    }

    _NYA_TwitchHelixCall call = { .kind = NYA_TWITCH_HELIX_CALL_UNSUBSCRIBE };

    _nya_twitch_helix_copy(call.subscription_id, sizeof(call.subscription_id), subscription_id);

    return _nya_twitch_helix_queue(helix, call, out_id);
}

NYA_Error nya_twitch_helix_chat_send(NYA_TwitchHelix* helix, NYA_ConstCString broadcaster_id, NYA_ConstCString text, u64* out_id) {
    nya_assert(helix != nullptr);

    if (broadcaster_id == nullptr || broadcaster_id[0] == '\0' || strlen(broadcaster_id) >= NYA_TWITCH_HELIX_MAX_ID) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message needs the channel it goes to");
    }

    if (text == nullptr || text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message with no text is not a message");
    if (strlen(text) >= NYA_TWITCH_HELIX_MAX_TEXT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the message is longer than twitch accepts");

    if (helix->bot_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message is sent as the bot, so bot_id is needed");

    _NYA_TwitchHelixCall call = { .kind = NYA_TWITCH_HELIX_CALL_CHAT_SEND };

    _nya_twitch_helix_copy(call.broadcaster_id, sizeof(call.broadcaster_id), broadcaster_id);
    _nya_twitch_helix_copy(call.text, sizeof(call.text), text);

    return _nya_twitch_helix_queue(helix, call, out_id);
}

b8 nya_twitch_helix_poll(NYA_TwitchHelix* helix, NYA_TwitchHelixResult* out_result) {
    nya_assert(helix != nullptr && out_result != nullptr);

    if (helix->queue_count == 0) return false;

    u64 now_ms = helix->now_ms(helix->user);

    _NYA_TwitchHelixCall call = helix->queue[0];

    if (now_ms < call.ready_at_ms) return false;

    // A bucket Twitch said is empty, until the reset it gave: asking anyway earns a 429, counted against the whole client id rather than this one call.
    if (helix->limit.reset_s != 0 && helix->limit.remaining == 0 && helix->now_s(helix->user) < helix->limit.reset_s) return false;

    b8 unsubscribing = call.kind == NYA_TWITCH_HELIX_CALL_UNSUBSCRIBE;

    NYA_ConstCString route = call.kind == NYA_TWITCH_HELIX_CALL_CHAT_SEND ? "/chat/messages" : "/eventsub/subscriptions";

    nya_arena_free_all(helix->exchanges);

    // The id goes in the delete's query, where Twitch takes it, and never in the route a result reports, which stays the bare path so a log line carries no ids.
    if (unsubscribing) {
        (void)snprintf(helix->url, sizeof(helix->url), "%s%s?id=%s", helix->base_url, route, call.subscription_id);
    } else {
        (void)snprintf(helix->url, sizeof(helix->url), "%s%s", helix->base_url, route);
    }
    (void)snprintf(helix->authorization, sizeof(helix->authorization), "Bearer %s", helix->token);

    NYA_Response response  = { 0 };
    NYA_Error    performed = helix->perform(helix->user, helix->exchanges,
                                            (NYA_Request){
                                                .method     = unsubscribing ? NYA_REQUEST_METHOD_DELETE : NYA_REQUEST_METHOD_POST,
                                                .url        = helix->url,
                                                .body       = unsubscribing ? nullptr : _nya_twitch_helix_body(helix, &call),
                                                .timeout_ms = helix->timeout_ms,
                                                .headers    = {
                                                    { .name = "Authorization", .value = helix->authorization },
                                                    { .name = "Client-Id", .value = helix->client_id },
                                                },
                                            },
                                            &response);

    // The header held the token, so it stops existing the moment the transfer is over.
    nya_crypto_wipe(helix->authorization, sizeof(helix->authorization));

    _nya_twitch_helix_observe(helix, &response);

    b8 retryable = response.status == 429 || (response.status >= 500 && response.status < 600);

    if (retryable && call.attempts + 1 < NYA_TWITCH_HELIX_MAX_ATTEMPTS) {
        helix->queue[0].attempts    += 1;
        helix->queue[0].ready_at_ms  = now_ms + ((u64)NYA_TWITCH_HELIX_RETRY_MS << call.attempts);

        return false;
    }

    _nya_twitch_helix_dequeue(helix, 0);

    _nya_twitch_helix_copy(helix->last_route, sizeof(helix->last_route), route);

    *out_result = (NYA_TwitchHelixResult){
        .id     = call.id,
        .kind   = call.kind,
        .route  = helix->last_route,
        .status = response.status,
        .body   = response.body,
        .error  = performed,
    };

    // The refusal a bot actually hits, in the words that fix it: Twitch's 401 is "Invalid OAuth token" whether it is expired, the wrong kind, or missing a scope.
    if (response.status == 401) {
        out_result->error = nya_error(NYA_ERROR_PERMISSION_DENIED,
                                      "twitch refused the token for %s: it must be a user token for the bot account, with the scope this call needs",
                                      route);
    }

    return true;
}

u32 nya_twitch_helix_pending(const NYA_TwitchHelix* helix) {
    nya_assert(helix != nullptr);

    return helix->queue_count;
}

NYA_TwitchHelixLimit nya_twitch_helix_limit(const NYA_TwitchHelix* helix) {
    nya_assert(helix != nullptr);

    return helix->limit;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_twitch_helix_perform(void* user, NYA_Arena* arena, NYA_Request request, NYA_Response* out_response) {
    (void)user;

    return nya_request_perform(arena, request, out_response);
}

u64 _nya_twitch_helix_now_ms(void* user) {
    (void)user;

    return nya_clock_get_monotonic_ms();
}

u64 _nya_twitch_helix_now_s(void* user) {
    (void)user;

    return nya_clock_get_timestamp_s();
}

void _nya_twitch_helix_copy(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(destination != nullptr && capacity > 0);

    destination[0] = '\0';

    if (text == nullptr) return;

    u64 length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(destination, text, length);
    destination[length] = '\0';
}

NYA_Error _nya_twitch_helix_queue(NYA_TwitchHelix* helix, _NYA_TwitchHelixCall call, u64* out_id) {
    if (helix->queue_count >= NYA_TWITCH_HELIX_MAX_QUEUE) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the twitch queue is full at %d calls", NYA_TWITCH_HELIX_MAX_QUEUE);
    }

    call.id = helix->next_id;

    helix->next_id                   += 1;
    helix->queue[helix->queue_count]  = call;
    helix->queue_count               += 1;

    if (out_id != nullptr) *out_id = call.id;

    return NYA_OK;
}

void _nya_twitch_helix_dequeue(NYA_TwitchHelix* helix, u32 index) {
    nya_assert(index < helix->queue_count);

    for (u32 i = index; i + 1 < helix->queue_count; i++) helix->queue[i] = helix->queue[i + 1];

    helix->queue_count                 -= 1;
    helix->queue[helix->queue_count]    = (_NYA_TwitchHelixCall){ 0 };
}

NYA_Object* _nya_twitch_helix_body(NYA_TwitchHelix* helix, const _NYA_TwitchHelixCall* call) {
    NYA_Object* body = nya_object_create(helix->exchanges);

    switch (call->kind) {
        case NYA_TWITCH_HELIX_CALL_SUBSCRIBE: {
            nya_object_add(body, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->type });
            nya_object_add(body, "version", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->version });

            // Who the subscription is about and who is listening: Twitch wants both for chat, the channel whose chat it is and the user whose token grants the read.
            NYA_Object* condition = nya_object_create(helix->exchanges);
            nya_object_add(condition, "broadcaster_user_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->broadcaster_id });
            nya_object_add(condition, "user_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = helix->bot_id });

            nya_object_add(body, "condition", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *condition });

            NYA_Object* transport = nya_object_create(helix->exchanges);
            nya_object_add(transport, "method", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"websocket" });
            nya_object_add(transport, "session_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->session });

            nya_object_add(body, "transport", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *transport });
            break;
        }

        // No body: the id is in the query, which is what Twitch takes for a delete.
        case NYA_TWITCH_HELIX_CALL_UNSUBSCRIBE: break;

        case NYA_TWITCH_HELIX_CALL_CHAT_SEND:
        case NYA_TWITCH_HELIX_CALL_KIND_COUNT:
        default: {
            nya_object_add(body, "broadcaster_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->broadcaster_id });
            nya_object_add(body, "sender_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = helix->bot_id });
            nya_object_add(body, "message", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)call->text });
            break;
        }
    }

    return body;
}

void _nya_twitch_helix_observe(NYA_TwitchHelix* helix, const NYA_Response* response) {
    // A transfer that never reached Twitch says nothing about the bucket, and reading zeroes out of it
    // would stop the client until a reset that was never given.
    if (response->status == 0) return;

    helix->limit.limit     = (u32)_nya_twitch_helix_header_u64(response, "Ratelimit-Limit", helix->limit.limit);
    helix->limit.remaining = (u32)_nya_twitch_helix_header_u64(response, "Ratelimit-Remaining", helix->limit.remaining);
    helix->limit.reset_s   = _nya_twitch_helix_header_u64(response, "Ratelimit-Reset", helix->limit.reset_s);

    // A 429 with no headers still means stop, so the bucket is emptied for a second rather than left
    // looking full because Twitch declined to say.
    if (response->status == 429 && helix->limit.reset_s == 0) {
        helix->limit.remaining = 0;
        helix->limit.reset_s   = helix->now_s(helix->user) + 1;
    }
}

u64 _nya_twitch_helix_header_u64(const NYA_Response* response, NYA_ConstCString name, u64 fallback) {
    char text[32] = { 0 };

    if (!nya_response_header(response, name, text, sizeof(text))) return fallback;

    u64 parsed = 0;
    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)text, strlen(text), &parsed)) return fallback;

    return parsed;
}
