#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest header value this reads. Every one of them is a number or a bucket hash. */
#define _NYA_DISCORD_MAX_HEADER_VALUE 64

/** Interaction callback type 4: a message the user sees, in the channel the command was used in. */
#define _NYA_DISCORD_CALLBACK_MESSAGE 4

typedef struct _NYA_DiscordRestEntry _NYA_DiscordRestEntry;

/**
 * One queued request, holding what it was asked with rather than a serialized body.
 *
 * Fixed fields and no pointers: the body is built into the poll's scratch arena at the moment it goes
 * out, so a request sitting in the queue for a minute holds no allocation open and the whole queue is
 * one flat block whose size is on the page.
 * */
struct _NYA_DiscordRestEntry {
    u64                 id;
    NYA_DiscordRestKind kind;

    char route[NYA_DISCORD_REST_MAX_ROUTE];
    char path[NYA_DISCORD_REST_MAX_PATH];
    char content[NYA_DISCORD_REST_MAX_CONTENT];
    char name[NYA_DISCORD_REST_MAX_NAME];
    char description[NYA_DISCORD_REST_MAX_DESCRIPTION];

    u32 attempts;

    /** Set by a retry, so a failing request waits rather than going out as fast as the caller polls. */
    u64 ready_at_ms;
};

struct NYA_DiscordRest {
    NYA_Arena* allocator;

    /** Where a body is serialized and a reply is parsed. Emptied at the top of every poll. */
    NYA_Arena* exchanges;

    char token[NYA_DISCORD_REST_MAX_TOKEN];
    char application_id[NYA_DISCORD_REST_MAX_ID];
    char base_url[NYA_DISCORD_REST_MAX_URL];
    u64  timeout_ms;

    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    void* user;

    _NYA_DiscordRestEntry queue[NYA_DISCORD_REST_MAX_QUEUE];
    u32                   queue_count;

    /** The route of the last result handed out, which outlives the entry it came from. */
    char last_route[NYA_DISCORD_REST_MAX_ROUTE];

    /** Where the url and the Authorization header of the request in flight are built, and wiped after. */
    char url[NYA_DISCORD_REST_MAX_URL + NYA_DISCORD_REST_MAX_PATH];
    char authorization[NYA_DISCORD_REST_MAX_TOKEN + 8];

    /** Handed out with every queued request, so a result can be matched to the call that made it. */
    u64 next_id;

    NYA_DiscordRateLimit limits;
};

/** The default transport: nya_request_perform, and the monotonic clock. */
NYA_INTERNAL NYA_Error _nya_discord_rest_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;
NYA_INTERNAL u64       _nya_discord_rest_now_ms(void* user) __attr_no_discard;

/** Finds the bucket `route` belongs to, or null. */
NYA_INTERNAL NYA_DiscordRateLimitBucket* _nya_discord_bucket_find(NYA_DiscordRateLimit* limits, NYA_ConstCString route) __attr_no_discard;

/** The bucket for `route`, made if it is new. Null when the table is full. */
NYA_INTERNAL NYA_DiscordRateLimitBucket* _nya_discord_bucket_intern(NYA_DiscordRateLimit* limits, NYA_ConstCString route) __attr_no_discard;

/** `header` as a whole number of milliseconds, from a value Discord writes as seconds with a fraction. */
NYA_INTERNAL b8 _nya_discord_header_ms(const NYA_Response* response, NYA_ConstCString header, OUT u64* out_ms) __attr_no_discard;

/** `header` as a whole number. */
NYA_INTERNAL b8 _nya_discord_header_u32(const NYA_Response* response, NYA_ConstCString header, OUT u32* out_value) __attr_no_discard;

/** Appends an entry to the queue and hands back its id. */
NYA_INTERNAL NYA_Error _nya_discord_rest_queue(NYA_DiscordRest* rest, const _NYA_DiscordRestEntry* entry, OUT u64* out_id) __attr_no_discard;

/** Drops the entry at `index`, keeping the rest in order. */
NYA_INTERNAL void _nya_discord_rest_dequeue(NYA_DiscordRest* rest, u32 index);

/** Builds the JSON body one entry needs, or null when it has none. */
NYA_INTERNAL NYA_Object* _nya_discord_rest_body(NYA_Arena* arena, const _NYA_DiscordRestEntry* entry) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * THE RATE LIMIT
 * ─────────────────────────────────────────────────────────
 */

void nya_discord_rate_limit_observe(NYA_DiscordRateLimit* limits, NYA_ConstCString route, u32 status, const NYA_Response* response, u64 now_ms) {
    nya_assert(limits != nullptr);
    nya_assert(route != nullptr);
    nya_assert(response != nullptr);

    char global[_NYA_DISCORD_MAX_HEADER_VALUE] = { 0 };
    b8   is_global                             = nya_response_header(response, "x-ratelimit-global", global, sizeof(global));

    u64 retry_after_ms = 0;
    b8  has_retry      = _nya_discord_header_ms(response, "retry-after", &retry_after_ms);

    /*
     * A global 429 is the whole token, not a route: nothing at all goes out until it passes. This is the
     * one Discord escalates to a ban when a client keeps pushing through it.
     */
    if (status == 429 && is_global && has_retry) {
        limits->global_reset_at_ms = now_ms + retry_after_ms;

        // The route is not named, and neither is the token. What a reader needs is that it happened.
        nya_log_warn("Discord rate limited this bot globally for %llu ms", (unsigned long long)retry_after_ms);
    }

    NYA_DiscordRateLimitBucket* bucket = _nya_discord_bucket_intern(limits, route);
    if (bucket == nullptr) return;

    char id[NYA_DISCORD_REST_MAX_BUCKET_ID] = { 0 };
    if (nya_response_header(response, "x-ratelimit-bucket", id, sizeof(id))) {
        nya_memcpy(bucket->id, id, sizeof(id));
    }

    u32 limit = 0;
    if (_nya_discord_header_u32(response, "x-ratelimit-limit", &limit)) bucket->limit = limit;

    u64 reset_after_ms = 0;
    if (_nya_discord_header_ms(response, "x-ratelimit-reset-after", &reset_after_ms)) bucket->reset_at_ms = now_ms + reset_after_ms;

    u32 remaining = 0;
    if (_nya_discord_header_u32(response, "x-ratelimit-remaining", &remaining)) bucket->remaining = remaining;

    /*
     * A 429 that is not global belongs to this bucket, and `Retry-After` is more trustworthy about it
     * than the reset the same reply carried: the reply is the server saying this bucket is spent now.
     */
    if (status == 429 && !is_global) {
        bucket->remaining = 0;
        if (has_retry) bucket->reset_at_ms = now_ms + retry_after_ms;
    }
}

b8 nya_discord_rate_limit_ready(const NYA_DiscordRateLimit* limits, NYA_ConstCString route, u64 now_ms, OUT u64* out_wait_ms) {
    nya_assert(limits != nullptr);
    nya_assert(route != nullptr);
    nya_assert(out_wait_ms != nullptr);

    *out_wait_ms = 0;

    if (now_ms < limits->global_reset_at_ms) {
        *out_wait_ms = limits->global_reset_at_ms - now_ms;
        return false;
    }

    const NYA_DiscordRateLimitBucket* bucket = _nya_discord_bucket_find((NYA_DiscordRateLimit*)limits, route);

    // Never seen, so nothing says it is spent. The first call is how the bucket gets learned at all.
    if (bucket == nullptr) return true;

    if (bucket->remaining > 0) return true;

    // Spent, and the window has passed: the server refills it whether or not it told this client so.
    if (now_ms >= bucket->reset_at_ms) return true;

    *out_wait_ms = bucket->reset_at_ms - now_ms;

    return false;
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_discord_rest_create(NYA_Arena* arena, NYA_DiscordRestOptions options, OUT NYA_DiscordRest** out_rest) {
    nya_assert(arena != nullptr);
    nya_assert(out_rest != nullptr);

    if (options.token == nullptr || options.token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a bot rest client needs a token");

    u64 token_length = strlen(options.token);

    if (token_length >= NYA_DISCORD_REST_MAX_TOKEN) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token is longer than %d bytes", NYA_DISCORD_REST_MAX_TOKEN - 1);
    }

    // The same rule the gateway applies, and for the same reason: this one goes into a header rather
    // than a JSON string, and a byte that does not belong in a token is how a header gets split.
    if (!_nya_discord_token_is_plain(options.token, token_length)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token carries a byte a real one never does");
    }

    NYA_ConstCString base_url = options.base_url != nullptr && options.base_url[0] != '\0' ? options.base_url : NYA_DISCORD_REST_URL;

    NYA_DiscordRest* rest = nya_arena_alloc(arena, sizeof(NYA_DiscordRest));
    *rest = (NYA_DiscordRest){
        .allocator  = arena,
        .exchanges  = nya_arena_create(.name = "discord_rest_exchanges"),
        .timeout_ms = options.timeout_ms,
        .perform    = options.perform != nullptr ? options.perform : _nya_discord_rest_perform,
        .now_ms     = options.now_ms != nullptr ? options.now_ms : _nya_discord_rest_now_ms,
        .user       = options.user,
        .next_id    = 1,
    };

    _nya_discord_copy(rest->token, sizeof(rest->token), options.token);
    _nya_discord_copy(rest->base_url, sizeof(rest->base_url), base_url);
    _nya_discord_copy(rest->application_id, sizeof(rest->application_id), options.application_id);

    *out_rest = rest;

    return NYA_OK;
}

void nya_discord_rest_destroy(NYA_DiscordRest* rest) {
    if (rest == nullptr) return;

    nya_crypto_wipe(rest->token, sizeof(rest->token));
    nya_crypto_wipe(rest->authorization, sizeof(rest->authorization));

    // The interaction tokens in the queued paths and in the last url are credentials too, and a dropped
    // queue should not leave them behind any more than the bot token.
    nya_crypto_wipe(rest->queue, sizeof(rest->queue));
    nya_crypto_wipe(rest->url, sizeof(rest->url));

    nya_arena_destroy(rest->exchanges);
    nya_arena_free(rest->allocator, rest, sizeof(NYA_DiscordRest));
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CALLS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_discord_rest_message_send(NYA_DiscordRest* rest, NYA_ConstCString channel_id, NYA_ConstCString content, OUT u64* out_id) {
    nya_assert(rest != nullptr);

    if (channel_id == nullptr || channel_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message needs a channel");
    if (content == nullptr || content[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message needs content");
    if (strlen(content) >= NYA_DISCORD_REST_MAX_CONTENT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the content is longer than %d bytes", NYA_DISCORD_REST_MAX_CONTENT - 1);
    }

    _NYA_DiscordRestEntry entry = { .kind = NYA_DISCORD_REST_MESSAGE_SEND };

    /*
     * The channel id is in the route as well as the path. Discord buckets per channel for this one, so
     * two channels must not share a bucket here or a busy channel would throttle a quiet one.
     */
    (void)snprintf(entry.route, sizeof(entry.route), "POST /channels/%s/messages", channel_id);
    (void)snprintf(entry.path, sizeof(entry.path), "/channels/%s/messages", channel_id);
    _nya_discord_copy(entry.content, sizeof(entry.content), content);

    return _nya_discord_rest_queue(rest, &entry, out_id);
}

NYA_Error nya_discord_rest_command_register(NYA_DiscordRest* rest, NYA_ConstCString name, NYA_ConstCString description, OUT u64* out_id) {
    nya_assert(rest != nullptr);

    if (rest->application_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no application id was configured, so there is nothing to register under");
    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command needs a name");
    if (strlen(name) >= NYA_DISCORD_REST_MAX_NAME) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the command name is longer than %d bytes", NYA_DISCORD_REST_MAX_NAME - 1);

    // Discord requires one and refuses the whole registration without it, which comes back as a 400 that
    // says nothing useful. Refusing here says what is missing.
    if (description == nullptr || description[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a command needs a description");
    if (strlen(description) >= NYA_DISCORD_REST_MAX_DESCRIPTION) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the command description is longer than %d bytes", NYA_DISCORD_REST_MAX_DESCRIPTION - 1);
    }

    _NYA_DiscordRestEntry entry = { .kind = NYA_DISCORD_REST_COMMAND_REGISTER };

    // The application id is this bot's and never changes, so the route is the same for every command:
    // one bucket, which is what Discord meters this on.
    _nya_discord_copy(entry.route, sizeof(entry.route), "POST /applications/{id}/commands");
    (void)snprintf(entry.path, sizeof(entry.path), "/applications/%s/commands", rest->application_id);
    _nya_discord_copy(entry.name, sizeof(entry.name), name);
    _nya_discord_copy(entry.description, sizeof(entry.description), description);

    return _nya_discord_rest_queue(rest, &entry, out_id);
}

NYA_Error nya_discord_rest_interaction_reply(NYA_DiscordRest* rest, NYA_ConstCString interaction_id, NYA_ConstCString interaction_token,
                                             NYA_ConstCString content, OUT u64* out_id) {
    nya_assert(rest != nullptr);

    if (interaction_id == nullptr || interaction_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a reply needs an interaction");
    if (interaction_token == nullptr || interaction_token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a reply needs the interaction's token");
    if (content == nullptr || content[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a reply needs content");
    if (strlen(content) >= NYA_DISCORD_REST_MAX_CONTENT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the content is longer than %d bytes", NYA_DISCORD_REST_MAX_CONTENT - 1);
    }

    _NYA_DiscordRestEntry entry = { .kind = NYA_DISCORD_REST_INTERACTION_REPLY };

    // The interaction token is in the path and deliberately not in the route: the route is what gets
    // logged, and a per interaction credential has no business in a log line.
    _nya_discord_copy(entry.route, sizeof(entry.route), "POST /interactions/{id}/{token}/callback");

    s32 written = snprintf(entry.path, sizeof(entry.path), "/interactions/%s/%s/callback", interaction_id, interaction_token);
    if (written < 0 || (u64)written >= sizeof(entry.path)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the interaction id and token do not fit in %d bytes", NYA_DISCORD_REST_MAX_PATH - 1);
    }

    _nya_discord_copy(entry.content, sizeof(entry.content), content);

    return _nya_discord_rest_queue(rest, &entry, out_id);
}

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

u32 nya_discord_rest_pending(const NYA_DiscordRest* rest) {
    nya_assert(rest != nullptr);

    return rest->queue_count;
}

const NYA_DiscordRateLimit* nya_discord_rest_limits(const NYA_DiscordRest* rest) {
    nya_assert(rest != nullptr);

    return &rest->limits;
}

b8 nya_discord_rest_poll(NYA_DiscordRest* rest, OUT NYA_DiscordRestResult* out_result) {
    nya_assert(rest != nullptr);
    nya_assert(out_result != nullptr);

    *out_result = (NYA_DiscordRestResult){ .route = "", .error = NYA_OK };

    if (rest->queue_count == 0) return false;

    // Last poll's body and reply die here, which is what the "valid until the next poll" in the header
    // means. Nothing in the queue points into it.
    nya_arena_free_all(rest->exchanges);

    u64 now_ms = rest->now_ms(rest->user);

    /*
     * The first entry whose bucket allows it, rather than only the head: one spent channel should not
     * hold up a command registration that shares nothing with it.
     */
    u32 index = rest->queue_count;
    for (u32 i = 0; i < rest->queue_count; i++) {
        if (now_ms < rest->queue[i].ready_at_ms) continue;

        u64 wait_ms = 0;
        if (nya_discord_rate_limit_ready(&rest->limits, rest->queue[i].route, now_ms, &wait_ms)) {
            index = i;
            break;
        }
    }

    // Everything queued is waiting on a bucket. There is nothing for the caller to do about that, so it
    // looks the same as an empty queue and costs nothing to ask again next frame.
    if (index == rest->queue_count) return false;

    _NYA_DiscordRestEntry entry = rest->queue[index];

    /*
     * Both built in the client's own buffers rather than the arena: the url carries an interaction
     * token and the header carries the bot token, and an arena copy of either would sit there until the
     * next poll emptied it — without being zeroed even then.
     */
    (void)snprintf(rest->url, sizeof(rest->url), "%s%s", rest->base_url, entry.path);

    // "Bot <token>", which is the bot API's scheme and not the bearer one nya_request_perform would add.
    (void)snprintf(rest->authorization, sizeof(rest->authorization), "Bot %s", rest->token);

    NYA_Response response = { 0 };
    NYA_Error    performed = rest->perform(rest->user, rest->exchanges,
                                           (NYA_Request){
                                               .method     = NYA_REQUEST_METHOD_POST,
                                               .url        = rest->url,
                                               .body       = _nya_discord_rest_body(rest->exchanges, &entry),
                                               .timeout_ms = rest->timeout_ms,
                                               .headers    = {
                                                   { .name = "Authorization", .value = rest->authorization },
                                                   // Discord asks every library to identify itself, and answers one that does not with a 403.
                                                   { .name = "User-Agent", .value = "DiscordBot (https://github.com/nyangine/nyangine, " NYA_VERSION ")" },
                                               },
                                           },
                                           &response);

    nya_crypto_wipe(rest->authorization, sizeof(rest->authorization));
    nya_crypto_wipe(rest->url, sizeof(rest->url));

    nya_discord_rate_limit_observe(&rest->limits, entry.route, response.status, &response, now_ms);

    b8 retryable = response.status == 429 || (response.status >= 500 && response.status < 600);

    if (retryable && entry.attempts + 1 < NYA_DISCORD_REST_MAX_ATTEMPTS) {
        // Left where it is rather than moved to the back: the rate limit decides when it goes, and
        // reordering a bot's messages to work around a 429 is worse than sending them a second late.
        rest->queue[index].attempts    += 1;
        rest->queue[index].ready_at_ms  = now_ms + ((u64)NYA_DISCORD_REST_RETRY_MS << entry.attempts);

        return false;
    }

    _nya_discord_rest_dequeue(rest, index);

    // Into the client rather than the scratch arena: the caller reads the result after the poll
    // returned, and the next poll is where the arena is emptied.
    _nya_discord_copy(rest->last_route, sizeof(rest->last_route), entry.route);

    *out_result = (NYA_DiscordRestResult){
        .id     = entry.id,
        .kind   = entry.kind,
        .route  = rest->last_route,
        .status = response.status,
        .body   = response.body,
        .error  = performed,
    };

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_discord_rest_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
    (void)user;

    return nya_request_perform(arena, request, out_response);
}

u64 _nya_discord_rest_now_ms(void* user) {
    (void)user;

    return nya_clock_get_monotonic_ms();
}

NYA_DiscordRateLimitBucket* _nya_discord_bucket_find(NYA_DiscordRateLimit* limits, NYA_ConstCString route) {
    for (u32 i = 0; i < limits->bucket_count; i++) {
        if (nya_string_equals((NYA_ConstCString)limits->buckets[i].route, route)) return &limits->buckets[i];
    }

    return nullptr;
}

NYA_DiscordRateLimitBucket* _nya_discord_bucket_intern(NYA_DiscordRateLimit* limits, NYA_ConstCString route) {
    NYA_DiscordRateLimitBucket* existing = _nya_discord_bucket_find(limits, route);
    if (existing != nullptr) return existing;

    if (limits->bucket_count >= NYA_DISCORD_REST_MAX_BUCKETS) {
        // Nothing is evicted: forgetting a bucket is forgetting that it is spent, which is exactly the
        // mistake that gets a token limited. A bot that touches this many routes wants a bigger table.
        nya_log_warn("The Discord rate limit table is full at %d buckets; '%s' is not being metered", NYA_DISCORD_REST_MAX_BUCKETS, route);
        return nullptr;
    }

    NYA_DiscordRateLimitBucket* bucket = &limits->buckets[limits->bucket_count];
    limits->bucket_count += 1;

    *bucket = (NYA_DiscordRateLimitBucket){ .limit = 1, .remaining = 1 };
    _nya_discord_copy(bucket->route, sizeof(bucket->route), route);

    return bucket;
}

b8 _nya_discord_header_ms(const NYA_Response* response, NYA_ConstCString header, OUT u64* out_ms) {
    *out_ms = 0;

    char value[_NYA_DISCORD_MAX_HEADER_VALUE] = { 0 };
    if (!nya_response_header(response, header, value, sizeof(value))) return false;

    char*  end     = nullptr;
    double seconds = strtod(value, &end);

    // A header that is not a number is a header this client does not act on, rather than a zero wait.
    if (end == value || seconds < 0.0) return false;

    *out_ms = (u64)(seconds * 1000.0);

    return true;
}

b8 _nya_discord_header_u32(const NYA_Response* response, NYA_ConstCString header, OUT u32* out_value) {
    *out_value = 0;

    char value[_NYA_DISCORD_MAX_HEADER_VALUE] = { 0 };
    if (!nya_response_header(response, header, value, sizeof(value))) return false;

    char* end    = nullptr;
    long  number = strtol(value, &end, 10);

    if (end == value || number < 0 || number > (long)UINT32_MAX) return false;

    *out_value = (u32)number;

    return true;
}

NYA_Error _nya_discord_rest_queue(NYA_DiscordRest* rest, const _NYA_DiscordRestEntry* entry, OUT u64* out_id) {
    nya_assert(out_id != nullptr);

    *out_id = 0;

    if (rest->queue_count >= NYA_DISCORD_REST_MAX_QUEUE) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the Discord request queue is full at %d", NYA_DISCORD_REST_MAX_QUEUE);
    }

    rest->queue[rest->queue_count]     = *entry;
    rest->queue[rest->queue_count].id  = rest->next_id;
    rest->queue_count                 += 1;

    *out_id = rest->next_id;
    rest->next_id += 1;

    return NYA_OK;
}

void _nya_discord_rest_dequeue(NYA_DiscordRest* rest, u32 index) {
    nya_assert(index < rest->queue_count);

    for (u32 i = index; i + 1 < rest->queue_count; i++) rest->queue[i] = rest->queue[i + 1];

    rest->queue_count -= 1;

    // The vacated slot held an interaction token in its path. Cleared rather than left for the next
    // entry to partially overwrite.
    nya_crypto_wipe(&rest->queue[rest->queue_count], sizeof(_NYA_DiscordRestEntry));
}

NYA_Object* _nya_discord_rest_body(NYA_Arena* arena, const _NYA_DiscordRestEntry* entry) {
    NYA_Object* body = nya_object_create(arena);

    switch (entry->kind) {
        case NYA_DISCORD_REST_MESSAGE_SEND:
            nya_object_add(body, "content", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)entry->content });
            break;

        case NYA_DISCORD_REST_COMMAND_REGISTER:
            nya_object_add(body, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)entry->name });
            nya_object_add(body, "description", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)entry->description });
            // Type 1 is CHAT_INPUT, which is what "slash command" means; the others are the right click
            // menu entries and take no description.
            nya_object_add(body, "type", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });
            break;

        case NYA_DISCORD_REST_INTERACTION_REPLY: {
            NYA_Object* data = nya_object_create(arena);
            nya_object_add(data, "content", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)entry->content });

            nya_object_add(body, "type", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = _NYA_DISCORD_CALLBACK_MESSAGE });
            nya_object_add(body, "data", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *data });
            break;
        }

        case NYA_DISCORD_REST_KIND_COUNT:
        default:                          nya_assert(false, "no body is defined for request kind %d", (int)entry->kind); break;
    }

    return body;
}
