/**
 * @file telegram.h
 *
 * A Telegram bot: the updates it is handed and the messages it sends back.
 *
 * Telegram is the third shape a bot API comes in and the plainest of the three. There is no gateway
 * socket and no subscription: a bot asks `getUpdates` for everything since the last one it
 * acknowledged, and sends with ordinary calls. What that buys is that a bot which was switched off
 * for an hour gets the hour back, because the server keeps an update until the bot has moved its
 * offset past it — a guarantee Discord's gateway does not make.
 *
 * ```c
 * NYA_Telegram* bot = nullptr;
 * NYA_EXPECT(nya_telegram_create(arena, (NYA_TelegramOptions){ .token = token }, &bot));
 * defer nya_telegram_destroy(bot);
 *
 * // once a frame
 * NYA_TelegramUpdate update = { 0 };
 * while (nya_telegram_poll(bot, &update)) {
 *     if (update.kind == NYA_TELEGRAM_UPDATE_MESSAGE && nya_string_equals(update.text, "/ping")) {
 *         NYA_EXPECT(nya_telegram_send(bot, update.chat_id, "pong"));
 *     }
 * }
 *
 * NYA_TelegramResult result = { 0 };
 * while (nya_telegram_result_poll(bot, &result)) {
 *     if (!result.error.ok) nya_log_warn("%s answered %u", result.method, result.status);
 * }
 * ```
 *
 * ── the offset, which is the whole protocol ──
 *
 * Every update carries an id. Asking for updates with `offset` set to one past the highest id seen
 * tells the server that everything below it has been dealt with, and only then may it forget them. So
 * the offset is moved when an update is handed to the caller, not when it is received: a batch that
 * arrives and is half read before the program dies is a batch that arrives again. The cost of that
 * choice is that a crash mid-batch can repeat an update, which is the direction to be wrong in for a
 * chat bot and the reason a handler should be able to see the same message twice without harm.
 *
 * ── long polling, and why the default is not to ──
 *
 * `getUpdates` takes a timeout: the server holds the request open that many seconds rather than
 * answering an empty list. That is how a bot gets a message the moment it is sent instead of at the
 * next poll, and it is also a transfer that does not return for that long. The transfers here are
 * synchronous, so a frame that calls into this would stop for the whole timeout: `poll_timeout_s` is
 * zero unless a caller sets it, and a caller sets it when this runs on a thread of its own.
 *
 * With it zero the poll asks no more often than NYA_TELEGRAM_POLL_INTERVAL_MS, because a bot that
 * asked every frame would be asking a hundred times a second for nothing.
 *
 * ── the rate limit ──
 *
 * Telegram publishes no headers to read: it answers 429 with `retry_after` seconds in the body, and
 * documents about thirty messages a second overall and one a second into any single chat. So the
 * client holds a cooldown rather than buckets — when a 429 arrives nothing is sent until it passes,
 * and the call that earned it is retried rather than dropped. The soft limits are the caller's to
 * respect; this refuses to make the hard one worse.
 *
 * ── the token ──
 *
 * The token *is* the URL here: Telegram puts it in the path rather than in a header, so every request
 * this makes has a secret in its url. That url is built in the client's own buffer and wiped after
 * the transfer, never into the arena a caller could read, and `method` in a result is the bare method
 * name for exactly this reason — it is what a log line gets to say.
 *
 * ── webhooks ──
 *
 * The other way in: Telegram POSTs each update to a url the bot registered. There is no signature,
 * only a secret the bot chose being echoed in `X-Telegram-Bot-Api-Secret-Token`, so
 * nya_telegram_webhook_verify compares it in constant time and that is all the proof there is. Use
 * https and a url nobody else knows; see http_webhook.h for the services that do sign.
 *
 * Thread safety: none. One thread owns a bot for its whole life.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-core/plugins/curl/request.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** Where the bot API lives. Overridable per client, which is what a test points at itself. */
#define NYA_TELEGRAM_URL "https://api.telegram.org"

/**
 * Bytes a token may take, terminator included.
 *
 * A token is `<bot id>:<35 characters>`, so this is twice the real thing and still small enough to
 * wipe. A longer one is refused at create rather than truncated, since half a token is a bot that
 * cannot log in and a message nobody can read.
 * */
#define NYA_TELEGRAM_MAX_TOKEN 128

/** A message's text, terminator included. Telegram refuses a longer one itself; this refuses it first. */
#define NYA_TELEGRAM_MAX_TEXT 4096

/** A display name or a username, terminator included. Telegram's own limit is 64 characters of UTF-8. */
#define NYA_TELEGRAM_MAX_NAME 128

/** A callback query's id, terminator included. */
#define NYA_TELEGRAM_MAX_ID 64

/**
 * Updates held between one answer and the calls that read them.
 *
 * `getUpdates` takes a limit and this is what it asks for, so a busier second is read over more
 * polls rather than into more memory. Past it the server keeps the rest, which is what the offset is
 * for.
 * */
#define NYA_TELEGRAM_MAX_UPDATES 16

/**
 * Calls waiting to be sent.
 *
 * One frame's worth of answers with room for a burst. Past it a send is refused with
 * NYA_ERROR_OUT_OF_MEMORY rather than growing, and a bot that fills this is a bot talking faster than
 * Telegram will take it.
 * */
#define NYA_TELEGRAM_MAX_QUEUE 32

/** How long the client waits between polls when it is not long polling. See the header's note. */
#define NYA_TELEGRAM_POLL_INTERVAL_MS 1000

/** What a failed call waits before the next attempt, doubled per attempt. */
#define NYA_TELEGRAM_RETRY_MS 500

/** How often one call is attempted before its failure is reported to the caller. */
#define NYA_TELEGRAM_MAX_ATTEMPTS 3

/** The header a webhook's secret arrives in. */
#define NYA_TELEGRAM_SECRET_HEADER "X-Telegram-Bot-Api-Secret-Token"

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef enum NYA_TelegramUpdateKind NYA_TelegramUpdateKind;
typedef enum NYA_TelegramCallKind   NYA_TelegramCallKind;
typedef struct NYA_TelegramUpdate   NYA_TelegramUpdate;
typedef struct NYA_TelegramOptions  NYA_TelegramOptions;
typedef struct NYA_TelegramResult   NYA_TelegramResult;
typedef struct NYA_Telegram         NYA_Telegram;

/** What one update turned out to be. An update this does not model is reported as _OTHER, never dropped silently. */
enum NYA_TelegramUpdateKind {
    /** Nothing. The zero, so a result nobody filled reads as no update rather than as a message. */
    NYA_TELEGRAM_UPDATE_NONE = 0,

    /** Somebody sent a message to a chat the bot is in. */
    NYA_TELEGRAM_UPDATE_MESSAGE,

    /** A message the bot had already seen was edited. */
    NYA_TELEGRAM_UPDATE_EDITED_MESSAGE,

    /** A button under a message was pressed. `callback_id` is what answers it. */
    NYA_TELEGRAM_UPDATE_CALLBACK_QUERY,

    /** Something else: a poll, a shipping query, a chat member change. The id still moves the offset. */
    NYA_TELEGRAM_UPDATE_OTHER,

    NYA_TELEGRAM_UPDATE_KIND_COUNT,
};

/** Which call a queued entry is, which is also what it is reported as. */
enum NYA_TelegramCallKind {
    NYA_TELEGRAM_CALL_SEND_MESSAGE = 0,
    NYA_TELEGRAM_CALL_ANSWER_CALLBACK,

    NYA_TELEGRAM_CALL_KIND_COUNT,
};

/** One thing that happened, as much of it as this models. */
struct NYA_TelegramUpdate {
    /** Telegram's own id for it. The offset is one past the highest of these that a caller has been handed. */
    u64 update_id;

    NYA_TelegramUpdateKind kind;

    /**
     * Which chat it happened in, and what nya_telegram_send answers into.
     *
     * Signed, and negative for a group or a channel: Telegram's own encoding, kept rather than
     * hidden, because a caller that stores one and a caller that reads the API documentation have to
     * be talking about the same number.
     * */
    s64 chat_id;

    /** Who did it. Zero when the update carries no sender, such as a channel post. */
    u64 from_id;

    /** Their display name, empty when the update carried none. */
    char from[NYA_TELEGRAM_MAX_NAME];

    /** The message's text, or a callback query's data. Empty for an update that carries neither. */
    char text[NYA_TELEGRAM_MAX_TEXT];

    /** The message this is, or the message a pressed button was under. Zero when there is none. */
    u64 message_id;

    /** What nya_telegram_answer_callback takes. Empty unless this is a callback query. */
    char callback_id[NYA_TELEGRAM_MAX_ID];
};

struct NYA_TelegramOptions {
    /** Required. The bot token from BotFather. Copied at create and wiped at destroy. */
    NYA_ConstCString token;

    /** The API root. Null means NYA_TELEGRAM_URL. */
    NYA_ConstCString base_url;

    /** What one transfer is given. Zero means NYA_REQUEST_DEFAULT_TIMEOUT_MS. */
    u64 timeout_ms;

    /**
     * Seconds the server may hold a poll open waiting for something to happen. Zero asks it to answer
     * at once, which is what a client on the frame thread wants; see the header.
     *
     * A timeout longer than `timeout_ms` would have the transfer give up before the server answers,
     * so create refuses that pair rather than polling in a way that can never succeed.
     * */
    u32 poll_timeout_s;

    /**
     * How a request is actually performed, and where the clock comes from.
     *
     * Left null, nya_request_perform and the monotonic clock. A test fills them with canned replies,
     * and a program that cannot afford a synchronous transfer in its frame fills them with its own.
     * */
    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    void* user;
};

/** What one queued call came to. */
struct NYA_TelegramResult {
    /** What the call that queued this returned. */
    u64 id;

    NYA_TelegramCallKind kind;

    /** The bare method name, such as "sendMessage". Safe to log: the token lives in the url, not here. */
    NYA_ConstCString method;

    /** The HTTP status, or zero when the transfer never got an answer. */
    u32 status;

    /** Ok when Telegram accepted it. Never carries the token, however it failed. */
    NYA_Error error;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/**
 * Makes a client. Nothing is sent and nothing is asked for until the first poll.
 *
 * Refuses a missing or oversized token, and a `poll_timeout_s` the transfer timeout could not outlive.
 * */
NYA_API NYA_Error nya_telegram_create(NYA_Arena* arena, NYA_TelegramOptions options, OUT NYA_Telegram** out_bot) __attr_no_discard;

/** Wipes the token and gives the arena back. Anything still queued is dropped unsent. */
NYA_API void nya_telegram_destroy(NYA_Telegram* bot);

/**
 * Hands over the next update, and asks for more when there are none and enough time has passed.
 *
 * False when there is nothing waiting, which is the answer most frames get. One call performs at most
 * one transfer, so a loop over this drains what arrived and then stops.
 * */
NYA_API b8 nya_telegram_poll(NYA_Telegram* bot, OUT NYA_TelegramUpdate* out_update);

/** Queues a message. The id it answers is what names it in a result. */
NYA_API NYA_Error nya_telegram_send(NYA_Telegram* bot, s64 chat_id, NYA_ConstCString text, OUT u64* out_id) __attr_no_discard;

/**
 * Queues the answer a pressed button is waiting for.
 *
 * Telegram shows a button as loading until this arrives, so a bot that never calls it looks broken
 * even when it did the work. `text` may be empty, which just stops the spinner.
 * */
NYA_API NYA_Error nya_telegram_answer_callback(NYA_Telegram* bot, NYA_ConstCString callback_id, NYA_ConstCString text, OUT u64* out_id) __attr_no_discard;

/**
 * Sends the next queued call that is due, and reports the one that finished.
 *
 * False when the queue is empty or the cooldown a 429 left has not passed. One call performs at most
 * one transfer, for the same reason the update poll does.
 * */
NYA_API b8 nya_telegram_result_poll(NYA_Telegram* bot, OUT NYA_TelegramResult* out_result);

/** How many calls are waiting, so a caller can stop queueing before the queue refuses one. */
NYA_API u32 nya_telegram_pending(const NYA_Telegram* bot) __attr_no_discard;

/** The offset the next poll will ask from: one past the highest update handed over. */
NYA_API u64 nya_telegram_offset(const NYA_Telegram* bot) __attr_no_discard;

/** Milliseconds until the client may send again, zero when it may now. What a 429 left behind. */
NYA_API u64 nya_telegram_cooldown_ms(const NYA_Telegram* bot, u64 now_ms) __attr_no_discard;

/**
 * Whether this request carries the secret the bot registered with its webhook.
 *
 * Constant time, and false for a request with no such header at all. It is the only proof Telegram
 * offers, so treat a true here as "this url has not leaked" and nothing stronger.
 * */
NYA_API b8 nya_telegram_webhook_verify(const NYA_HttpExchange* exchange, NYA_ConstCString secret) __attr_no_discard;

/**
 * Reads one update out of a parsed webhook body, which is the same object `getUpdates` returns in its
 * array. False for a body that is not an update.
 * */
NYA_API b8 nya_telegram_update_read(const NYA_Object* object, OUT NYA_TelegramUpdate* out_update);
