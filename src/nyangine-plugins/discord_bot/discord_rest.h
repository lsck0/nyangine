/**
 * @file discord_rest.h
 *
 * The other half of a Discord bot: the HTTP API it talks back through. The gateway says what happened;
 * this is how a bot answers.
 *
 * Enough of it to be a bot and no more: send a message to a channel, register a slash command, and
 * answer an interaction. Every one of them is a queue entry, and the queue is drained by a poll that
 * obeys the rate limit headers Discord answers with.
 *
 * ```c
 * NYA_DiscordRest* rest = nullptr;
 * NYA_EXPECT(nya_discord_rest_create(arena, (NYA_DiscordRestOptions){ .token = token, .application_id = app }, &rest));
 * defer nya_discord_rest_destroy(rest);
 *
 * NYA_EXPECT(nya_discord_rest_message_send(rest, channel_id, "pong"));
 *
 * // once a frame, beside the gateway's poll
 * NYA_DiscordRestResult result = { 0 };
 * while (nya_discord_rest_poll(rest, &result)) {
 *     if (!result.error.ok) nya_log_warn("%s answered %u", result.route, result.status);
 * }
 * ```
 *
 * ── the rate limits, and why they are not optional ──
 *
 * Discord rate limits per *bucket*, not per route: several routes can share one, and which ones do is
 * something only the server knows, so it names the bucket in `X-RateLimit-Bucket` on every reply. The
 * other three headers say how many calls are left in it (`X-RateLimit-Remaining`) and how long until it
 * refills (`X-RateLimit-Reset-After`). A 429 carries `Retry-After` and, when the whole bot is limited
 * rather than one bucket, `X-RateLimit-Global`.
 *
 * A client that ignores all of that does not get a slower bot, it gets a banned one: Discord answers
 * repeated 429s across the API with a global limit on the whole token, and then with a temporary ban on
 * the IP. So nya_discord_rest_poll refuses to send into a bucket that is spent and refuses to send
 * anything at all while a global limit is running. It refuses by returning — the wait is never a sleep,
 * because a bot that sleeps is a frame that dropped.
 *
 * The bookkeeping is nya_discord_rate_limit_observe and nya_discord_rate_limit_ready, over a plain
 * struct with no allocation in it, so the awkward part is testable without a network. See
 * tests/nyangine/plugins/test_discord_rest.c.
 *
 * ── what this does block on ──
 *
 * One transfer. nya_request_perform is synchronous, so the poll that decides a request may go then waits
 * for the answer, and that is milliseconds of a frame. Everything else — the queueing, the buckets, the
 * backoff, the decision not to send — is free and returns at once.
 *
 * That is a real limit and not a detail: a bot hosted inside a game should keep the request rate low, or
 * run this on a thread of its own, or hand the transport a `perform` of its own through the options.
 * Making the transfers themselves asynchronous means curl's multi interface, which is what
 * `plugins/curl/websocket.c` already drives and what this would grow into; it is not written yet.
 *
 * ── the token ──
 *
 * Copied at create, sent as `Authorization: Bot <token>`, wiped at destroy, and never in a log line or
 * an error message. Read the gateway header's section on what a memory read gets: everything it says
 * applies here, plus libcurl's own copy of the header inside its handle, which nothing in this engine
 * can reach.
 *
 * Thread safety: none. One thread owns a client for its whole life.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-plugins/curl/request.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** Where the bot API lives. Version 10, the same one the gateway speaks. */
#define NYA_DISCORD_REST_URL "https://discord.com/api/v10"

/** Token bytes held, terminator included. The gateway's bound, for the same reason. */
#define NYA_DISCORD_REST_MAX_TOKEN 128

/** API root bytes, terminator included. */
#define NYA_DISCORD_REST_MAX_URL 128

/** Snowflake bytes, terminator included. A Discord id is a 64 bit number in decimal, so never past 20 digits. */
#define NYA_DISCORD_REST_MAX_ID 24

/**
 * Requests queued before a new one is refused.
 *
 * Sixteen, because the queue exists to hold work while a bucket refills and not to be a spool: a bot
 * that is more than sixteen requests behind is asking for more than Discord will give it, and finding
 * that out as NYA_ERROR_OUT_OF_MEMORY at the call site beats finding it out as a 429 an hour later.
 * */
#define NYA_DISCORD_REST_MAX_QUEUE 16

/** Rate limit buckets remembered. A bot that only sends and answers touches a handful. */
#define NYA_DISCORD_REST_MAX_BUCKETS 32

/** Route key bytes, terminator included: the method and the path with the ids taken out. */
#define NYA_DISCORD_REST_MAX_ROUTE 96

/** Bucket id bytes, terminator included. Discord's is a 32 character hash. */
#define NYA_DISCORD_REST_MAX_BUCKET_ID 48

/**
 * Request path bytes, terminator included.
 *
 * Sized for the longest of the three shapes here, `/interactions/{id}/{token}/callback`, whose token is
 * a few hundred characters.
 * */
#define NYA_DISCORD_REST_MAX_PATH 512

/**
 * Message content bytes, terminator included.
 *
 * Bytes, not characters: Discord's limit is 2000 characters, and a message of nothing but four byte
 * codepoints would need 8000 of them. This is the buffer, and content that does not fit is refused at
 * the call rather than cut, because cutting UTF-8 in the middle of a codepoint produces a body Discord
 * rejects and a caller cannot see why.
 * */
#define NYA_DISCORD_REST_MAX_CONTENT 2048

/** Slash command name bytes. Discord's limit is 32 characters, all of them ASCII. */
#define NYA_DISCORD_REST_MAX_NAME 48

/** Slash command description bytes. Discord's limit is 100 characters. */
#define NYA_DISCORD_REST_MAX_DESCRIPTION 256

/**
 * How many times one request is retried before it is given back as failed.
 *
 * A 429 and a 5xx are retried; nothing else is. Three is enough to ride out a bucket that was already
 * spent when the request was queued, and few enough that a request which is simply wrong comes back
 * quickly instead of being retried into a rate limit of its own.
 * */
#define NYA_DISCORD_REST_MAX_ATTEMPTS 3

/**
 * How long a retried request waits before it is tried again, doubling per attempt.
 *
 * A 429 is held back by its bucket, which is the server telling this client exactly how long to wait. A
 * 5xx carries no such thing, and without this the retry would go out as fast as the caller polls — which
 * is a bot hammering an API that has just said it is having trouble.
 * */
#define NYA_DISCORD_REST_RETRY_MS 1000

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_DiscordRateLimitBucket NYA_DiscordRateLimitBucket;
typedef struct NYA_DiscordRateLimit       NYA_DiscordRateLimit;
typedef struct NYA_DiscordRestOptions     NYA_DiscordRestOptions;
typedef struct NYA_DiscordRestResult      NYA_DiscordRestResult;
typedef struct NYA_DiscordRest            NYA_DiscordRest;

/** One bucket as the last reply from it described it. */
struct NYA_DiscordRateLimitBucket {
    /** The route this was learned from, which is the key until a reply names the real bucket. */
    char route[NYA_DISCORD_REST_MAX_ROUTE];

    /** What Discord calls it in `X-RateLimit-Bucket`. Empty until a reply carried one. */
    char id[NYA_DISCORD_REST_MAX_BUCKET_ID];

    /** Calls the bucket holds, and how many are left in this window. */
    u32 limit;
    u32 remaining;

    /** When `remaining` goes back to `limit`, on the same monotonic clock the caller passes in. */
    u64 reset_at_ms;
};

/**
 * Every bucket, plus the one limit that is not per bucket.
 *
 * A plain struct with a fixed table in it: no allocation, no lifetime, and therefore something a test
 * can drive straight through nya_discord_rate_limit_observe without a client or a socket around it.
 * */
struct NYA_DiscordRateLimit {
    NYA_DiscordRateLimitBucket buckets[NYA_DISCORD_REST_MAX_BUCKETS];
    u32                        bucket_count;

    /** A 429 with `X-RateLimit-Global` stops everything until here, whichever bucket it was for. */
    u64 global_reset_at_ms;
};

/** Which of the three calls a queue entry is, which decides the method, the path and the body. */
typedef enum {
    /** POST /channels/{id}/messages */
    NYA_DISCORD_REST_MESSAGE_SEND = 0,

    /** POST /applications/{id}/commands, which upserts a global slash command by name. */
    NYA_DISCORD_REST_COMMAND_REGISTER,

    /** POST /interactions/{id}/{token}/callback, the answer to a slash command. */
    NYA_DISCORD_REST_INTERACTION_REPLY,

    NYA_DISCORD_REST_KIND_COUNT,
} NYA_DiscordRestKind;

struct NYA_DiscordRestOptions {
    /**
     * Required. The bot token, without the "Bot " prefix. Copied at create and wiped at destroy.
     * */
    NYA_ConstCString token;

    /**
     * The bot's application id, which is what a slash command is registered under.
     *
     * Only nya_discord_rest_command_register needs it; leaving it out is legal and makes that one call
     * answer NYA_ERROR_INVALID_ARGUMENT.
     * */
    NYA_ConstCString application_id;

    /** The API root. Null means NYA_DISCORD_REST_URL. */
    NYA_ConstCString base_url;

    /** What one transfer is given. Zero means NYA_REQUEST_DEFAULT_TIMEOUT_MS. */
    u64 timeout_ms;

    /**
     * How a request is actually performed, and where the clock comes from.
     *
     * Left null, nya_request_perform and the monotonic clock. A test fills them with canned replies, and
     * a program that cannot afford a synchronous transfer in its frame fills them with its own.
     * */
    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    void* user;
};

struct NYA_DiscordRestResult {
    /** What nya_discord_rest_message_send and its siblings returned when they queued this. */
    u64 id;

    NYA_DiscordRestKind kind;

    /** The route it went to, with the ids taken out. Safe to log; it carries no token. */
    NYA_ConstCString route;

    /** The HTTP status, or zero when the transfer never got an answer. */
    u32 status;

    /** The reply body, parsed. Null when there was none. Valid until the next poll. */
    const NYA_Object* body;

    /** Ok when Discord accepted it. Never carries the token, however it failed. */
    NYA_Error error;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

// ───────────────────────────────────── THE RATE LIMIT ─────────────────────────────────────

/**
 * Folds one reply's rate limit headers into `limits`.
 *
 * Reads `X-RateLimit-Bucket`, `-Remaining` and `-Reset-After` from any status, and `Retry-After` and
 * `X-RateLimit-Global` from a 429. A reply with none of them leaves the bucket alone rather than
 * guessing, since a proxy that strips headers should not look like a bucket that emptied.
 *
 * `now_ms` is whatever monotonic clock the caller uses; every deadline it stores is on that clock.
 * */
NYA_API void nya_discord_rate_limit_observe(NYA_DiscordRateLimit* limits, NYA_ConstCString route, u32 status, const NYA_Response* response, u64 now_ms);

/**
 * Whether a request on `route` may go out now, and how long until it may when it may not.
 *
 * `out_wait_ms` is zero when the answer is yes. A route that has never been seen is ready: the first
 * call to it is how the bucket gets learned.
 * */
NYA_API b8 nya_discord_rate_limit_ready(const NYA_DiscordRateLimit* limits, NYA_ConstCString route, u64 now_ms, OUT u64* out_wait_ms) __attr_no_discard;

// ───────────────────────────────────── LIFETIME ─────────────────────────────────────

/**
 * Copies the token, takes the queue from `arena`, and returns a client that has sent nothing yet.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a missing or malformed token, on the same rules the gateway applies.
 * */
NYA_API NYA_Error nya_discord_rest_create(NYA_Arena* arena, NYA_DiscordRestOptions options, OUT NYA_DiscordRest** out_rest) __attr_no_discard;

/** Wipes the token and frees the client, dropping anything still queued. Null is a no-op. */
NYA_API void nya_discord_rest_destroy(NYA_DiscordRest* rest);

// ───────────────────────────────────── THE CALLS ─────────────────────────────────────

/**
 * Queues "post `content` to channel `channel_id`" and answers the id its result will carry.
 *
 * NYA_ERROR_OUT_OF_MEMORY when the queue is full, NYA_ERROR_INVALID_ARGUMENT when the content is empty
 * or longer than NYA_DISCORD_REST_MAX_CONTENT.
 * */
NYA_API NYA_Error nya_discord_rest_message_send(NYA_DiscordRest* rest, NYA_ConstCString channel_id, NYA_ConstCString content, OUT u64* out_id)
    __attr_no_discard;

/**
 * Queues a global slash command under the configured application id.
 *
 * Discord upserts by name, so registering the same name twice updates it rather than making a second
 * one. A global command takes up to an hour to appear in every server; a guild scoped one is immediate
 * and is not implemented here.
 * */
NYA_API NYA_Error nya_discord_rest_command_register(NYA_DiscordRest* rest, NYA_ConstCString name, NYA_ConstCString description, OUT u64* out_id)
    __attr_no_discard;

/**
 * Queues the answer to an interaction: type 4, a message the user sees in the channel.
 *
 * Discord gives three seconds from the interaction arriving to this reaching it, which is short enough
 * that it is worth queueing this before anything slow. `interaction_token` is a credential for that one
 * interaction and is treated like one: it goes in the url and is never logged.
 * */
NYA_API NYA_Error nya_discord_rest_interaction_reply(NYA_DiscordRest* rest, NYA_ConstCString interaction_id, NYA_ConstCString interaction_token,
                                                     NYA_ConstCString content, OUT u64* out_id) __attr_no_discard;

// ───────────────────────────────────── OPERATIONS ─────────────────────────────────────

/**
 * Performs at most one queued request whose bucket allows it, and hands back its result.
 *
 * False when there is nothing queued, or when everything queued is waiting on a bucket — which is the
 * case a caller does nothing about, so it looks the same as an empty queue. Returns immediately in both.
 * When it does send, it waits for that one transfer; see the header on what blocks.
 * */
NYA_API b8 nya_discord_rest_poll(NYA_DiscordRest* rest, OUT NYA_DiscordRestResult* out_result);

/** How many requests are queued, sent or waiting. What a caller watches to know it is falling behind. */
NYA_API u32 nya_discord_rest_pending(const NYA_DiscordRest* rest) __attr_no_discard;

/** The client's rate limit state, for a log line or an overlay. Never null. */
NYA_API const NYA_DiscordRateLimit* nya_discord_rest_limits(const NYA_DiscordRest* rest) __attr_no_discard;
