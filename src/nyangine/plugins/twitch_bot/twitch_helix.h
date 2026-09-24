/**
 * @file twitch_helix.h
 *
 * A Twitch bot's voice: the HTTP API it subscribes through and answers with.
 *
 * `twitch_eventsub.h` opens the socket and hands out a session; nothing arrives on it until this file
 * has told Twitch what to send. It also carries chat, which used to mean IRC and does not any more:
 * `channel.chat.message` comes over EventSub and a reply is `POST /helix/chat/messages`, so a chat bot
 * needs no second protocol and no TLS of its own.
 *
 * ```c
 * NYA_TwitchHelix* helix = nullptr;
 * NYA_EXPECT(nya_twitch_helix_create(arena, (NYA_TwitchHelixOptions){
 *     .token     = user_token,          // a *user* token, not an app one; see below
 *     .client_id = client_id,
 *     .bot_id    = bot_user_id,
 * }, &helix));
 * defer nya_twitch_helix_destroy(helix);
 *
 * // on every NYA_TWITCH_EVENTSUB_WELCOME, because a new session has no subscriptions
 * NYA_EXPECT(nya_twitch_helix_subscribe(helix, "channel.chat.message", "1", broadcaster_id, session, nullptr));
 *
 * // and to say something
 * NYA_EXPECT(nya_twitch_helix_chat_send(helix, broadcaster_id, "pong", nullptr));
 *
 * // once a frame, beside the eventsub poll
 * NYA_TwitchHelixResult result = { 0 };
 * while (nya_twitch_helix_poll(helix, &result)) {
 *     if (!result.error.ok) nya_log_warn("%s answered %u", result.route, result.status);
 * }
 * ```
 *
 * ── which token ──
 *
 * Twitch has two and they are not interchangeable. An *app* token says "this application"; a *user*
 * token says "this account, having agreed to these scopes". Reading a chat, writing to one, and
 * subscribing to anything about a channel all need a user token with the right scopes
 * (`user:read:chat`, `user:write:chat`), and Twitch answers an app token used for them with a 401
 * whose message says only "Invalid OAuth token". So the token goes in with `Authorization: Bearer`,
 * the client id beside it in `Client-Id`, and a 401 here is reported as what it usually is: the wrong
 * kind of token, or one whose scopes were never granted.
 *
 * This file does not mint tokens. Getting one is a browser flow ending at a redirect url, which
 * belongs to whatever program owns a browser, and refreshing one is `POST /oauth2/token` against a
 * client secret this deliberately never holds.
 *
 * ── the rate limit ──
 *
 * Twitch publishes a bucket in headers — `Ratelimit-Limit`, `Ratelimit-Remaining`, `Ratelimit-Reset`
 * (a unix timestamp) — one bucket per client id across the whole API, plus separate per-channel limits
 * on chat that it does not publish at all. So this reads the bucket it is told about, stops sending
 * when it is spent until the reset it was given, and leaves the chat limits to the caller: twenty
 * messages in thirty seconds per channel is a rule about what a bot should say, not one a client can
 * enforce without deciding which message to drop.
 *
 * ── the queue ──
 *
 * Every call is queued and sent by the poll, in order, because a bot's calls are a conversation and
 * reordering them to route around a limit is worse than sending one a second late. A transfer is
 * synchronous, so a poll costs one round trip on the calling thread; a program that cannot afford that
 * in its frame gives the client a `perform` of its own or runs it on a thread.
 *
 * Thread safety: none. One thread owns a client for its whole life.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/plugins/curl/request.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** Where the API lives. Overridable per client, which is what a test points at itself. */
#define NYA_TWITCH_HELIX_URL "https://api.twitch.tv/helix"

/** A token, terminator included. Twitch's are thirty characters; this is room for what it grows into. */
#define NYA_TWITCH_HELIX_MAX_TOKEN 128

/** A user id, a client id or a subscription version, terminator included. */
#define NYA_TWITCH_HELIX_MAX_ID 64

/** A subscription type such as "channel.chat.message", terminator included. */
#define NYA_TWITCH_HELIX_MAX_TYPE 64

/** A session id from the EventSub welcome, terminator included. */
#define NYA_TWITCH_HELIX_MAX_SESSION 128

/**
 * A chat message's text, terminator included.
 *
 * Twitch's own limit is 500 characters. A longer one is refused here rather than truncated, because
 * half a message sent in a bot's name is worse than none.
 * */
#define NYA_TWITCH_HELIX_MAX_TEXT 512

/** The route as a result names it, terminator included. Ids are not in it, so it is safe to log. */
#define NYA_TWITCH_HELIX_MAX_ROUTE 64

/**
 * Calls waiting to be sent.
 *
 * A welcome's worth of subscriptions plus a burst of chat. Past it a call is refused with
 * NYA_ERROR_OUT_OF_MEMORY rather than growing.
 * */
#define NYA_TWITCH_HELIX_MAX_QUEUE 32

/** What a failed call waits before the next attempt, doubled per attempt. */
#define NYA_TWITCH_HELIX_RETRY_MS 500

/** How often one call is attempted before its failure is reported to the caller. */
#define NYA_TWITCH_HELIX_MAX_ATTEMPTS 3

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef enum NYA_TwitchHelixCallKind  NYA_TwitchHelixCallKind;
typedef struct NYA_TwitchHelixLimit   NYA_TwitchHelixLimit;
typedef struct NYA_TwitchHelixOptions NYA_TwitchHelixOptions;
typedef struct NYA_TwitchHelixResult  NYA_TwitchHelixResult;
typedef struct NYA_TwitchHelix        NYA_TwitchHelix;

/** Which call a queued entry is, which is also what it is reported as. */
enum NYA_TwitchHelixCallKind {
    /** POST /eventsub/subscriptions: tell the session what to send. */
    NYA_TWITCH_HELIX_CALL_SUBSCRIBE = 0,

    /** DELETE /eventsub/subscriptions: stop hearing about something. */
    NYA_TWITCH_HELIX_CALL_UNSUBSCRIBE,

    /** POST /chat/messages: say something in a channel. */
    NYA_TWITCH_HELIX_CALL_CHAT_SEND,

    NYA_TWITCH_HELIX_CALL_KIND_COUNT,
};

/** The bucket as the last reply described it. One per client id, across the whole API. */
struct NYA_TwitchHelixLimit {
    /** Calls the bucket holds, and how many are left in this window. */
    u32 limit;
    u32 remaining;

    /** Seconds since the epoch when it refills, from `Ratelimit-Reset`. Zero until a reply carried one. */
    u64 reset_s;
};

struct NYA_TwitchHelixOptions {
    /**
     * Required. A *user* access token, without the "Bearer " prefix. Copied at create and wiped at
     * destroy. See the header on which kind Twitch wants.
     * */
    NYA_ConstCString token;

    /** Required. The application's client id, which Twitch wants beside the token in every call. */
    NYA_ConstCString client_id;

    /**
     * The bot account's own user id: who a message is sent as, and whose token the subscription is
     * made with. Only the calls that need it are refused when it is missing.
     * */
    NYA_ConstCString bot_id;

    /** The API root. Null means NYA_TWITCH_HELIX_URL. */
    NYA_ConstCString base_url;

    /** What one transfer is given. Zero means NYA_REQUEST_DEFAULT_TIMEOUT_MS. */
    u64 timeout_ms;

    /**
     * How a request is actually performed, and where the clocks come from.
     *
     * Left null, nya_request_perform and the engine's clocks. A test fills them with canned replies,
     * and a program that cannot afford a synchronous transfer in its frame fills them with its own.
     * */
    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    u64 (*now_s)(void* user);
    void* user;
};

/** What one queued call came to. */
struct NYA_TwitchHelixResult {
    /** What the call that queued this returned. */
    u64 id;

    NYA_TwitchHelixCallKind kind;

    /** The route it went to, with no ids in it. Safe to log: it carries no token. */
    NYA_ConstCString route;

    /** The HTTP status, or zero when the transfer never got an answer. */
    u32 status;

    /** The reply body, parsed. Null when there was none. Valid until the next poll. */
    const NYA_Object* body;

    /** Ok when Twitch accepted it. Never carries the token, however it failed. */
    NYA_Error error;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/** Makes a client. Refuses a missing token or client id; nothing is sent until the first poll. */
NYA_API NYA_Error nya_twitch_helix_create(NYA_Arena* arena, NYA_TwitchHelixOptions options, OUT NYA_TwitchHelix** out_helix) __attr_no_discard;

/** Wipes the token and gives the arena back. Anything still queued is dropped unsent. */
NYA_API void nya_twitch_helix_destroy(NYA_TwitchHelix* helix);

/**
 * Queues a subscription: what to hear about, which version of it, whose channel, and the session it
 * should arrive on.
 *
 * `session` is what nya_twitch_eventsub_session answers, and a subscription belongs to that session
 * alone: every welcome means subscribing again. `version` is Twitch's own per-type version, "1" for
 * most types today, and it is a string because Twitch's are.
 * */
NYA_API NYA_Error nya_twitch_helix_subscribe(NYA_TwitchHelix* helix, NYA_ConstCString type, NYA_ConstCString version, NYA_ConstCString broadcaster_id,
                                             NYA_ConstCString session, OUT u64* out_id) __attr_no_discard;

/**
 * Queues the opposite: Twitch stops sending this subscription.
 *
 * `subscription_id` is the `data[0].id` the subscribe call's reply carried. A session that ends takes
 * its subscriptions with it, so this is for a bot that stops caring about a channel while it keeps
 * running — leaving one behind costs a slot against the limit Twitch counts per client id.
 * */
NYA_API NYA_Error nya_twitch_helix_unsubscribe(NYA_TwitchHelix* helix, NYA_ConstCString subscription_id, OUT u64* out_id) __attr_no_discard;

/** Queues a chat message into a channel, sent as the bot account `bot_id` names. */
NYA_API NYA_Error nya_twitch_helix_chat_send(NYA_TwitchHelix* helix, NYA_ConstCString broadcaster_id, NYA_ConstCString text, OUT u64* out_id)
    __attr_no_discard;

/**
 * Sends the next queued call that is due, and reports the one that finished.
 *
 * False when the queue is empty or the published bucket is spent. One call performs at most one
 * transfer.
 * */
NYA_API b8 nya_twitch_helix_poll(NYA_TwitchHelix* helix, OUT NYA_TwitchHelixResult* out_result);

/** How many calls are waiting, so a caller can stop queueing before the queue refuses one. */
NYA_API u32 nya_twitch_helix_pending(const NYA_TwitchHelix* helix) __attr_no_discard;

/** The bucket as the last reply described it. What an overlay shows and a log line quotes. */
NYA_API NYA_TwitchHelixLimit nya_twitch_helix_limit(const NYA_TwitchHelix* helix) __attr_no_discard;
