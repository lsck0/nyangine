/**
 * @file examples/twitch_bot/main.c
 *
 * A Twitch chat bot: a socket that is told what to send, and the calls that tell it.
 *
 * ```
 * ./build run example twitch_bot                       # says what is missing and stops
 * TWITCH_BOT_TOKEN=... TWITCH_CLIENT_ID=... TWITCH_BOT_ID=... TWITCH_CHANNEL_ID=... ./twitch_bot.example
 * ```
 *
 * It answers `!ping` in the channel it was pointed at, and logs everything else it hears.
 *
 * ## Running it with nothing set
 *
 * This is what CI does. A bot with no credentials is not a failure — there is nothing wrong with the
 * build — so it says what is missing, says where to get it, and exits zero.
 *
 * ## What the four variables are
 *
 * - `TWITCH_BOT_TOKEN`: a **user** access token for the bot's own account, with the `user:read:chat`
 *   and `user:write:chat` scopes. Not an app token; Twitch refuses those for chat with a 401 that says
 *   only "Invalid OAuth token", which is why this client rewrites that refusal into a sentence naming
 *   the cause.
 * - `TWITCH_CLIENT_ID`: the application the token was minted for. Twitch wants it in every call beside
 *   the token.
 * - `TWITCH_BOT_ID`: the bot account's numeric user id. It is who a message is sent as.
 * - `TWITCH_CHANNEL_ID`: the numeric user id of the channel to listen to. Not the name — Twitch's API
 *   takes ids, and turning a name into one is a `GET /helix/users` this example does not need.
 *
 * Getting a token is a browser flow that ends at a redirect url, so it belongs to a program that owns a
 * browser rather than to the engine; the token goes in the environment, never in a file in this tree.
 *
 * ## The two halves, and why they are separate
 *
 * The socket knows what happened; the Helix client knows what the bot wants to do about it and what
 * Twitch's rate limit will let it. Nothing is pushed down the socket until a subscription has been made
 * against the session it hands out, and **every** welcome is a new session — including the one after a
 * reconnect, which is why subscribing lives in the welcome case below rather than in start-up.
 *
 * Neither poll blocks this loop for longer than one HTTP transfer, which is why this shape drops
 * straight into a game's frame: a game that wants a chat bot is this file's loop body, once a frame.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include <signal.h>

/* CONSTANTS */

#define TOKEN_VARIABLE   "TWITCH_BOT_TOKEN"
#define CLIENT_VARIABLE  "TWITCH_CLIENT_ID"
#define BOT_VARIABLE     "TWITCH_BOT_ID"
#define CHANNEL_VARIABLE "TWITCH_CHANNEL_ID"

/** What the bot listens for, and the version of it Twitch is at today. */
#define CHAT_SUBSCRIPTION "channel.chat.message"
#define CHAT_VERSION      "1"

/** How long the loop sleeps between polls. Nothing here is on a frame budget, so this is generous. */
#define TICK_SLEEP_MS 20

/** The one command, answered wherever the bot can read it. */
#define PING_COMMAND "!ping"

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
static volatile sig_atomic_t RUNNING = 1;

static void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

/* THE HANDLERS */

/** `object[key]` as a string, or null. The payloads are documents, so everything optional is missing. */
static NYA_ConstCString string_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

/** `object[key]` as a nested object, or null. */
static const NYA_Object* object_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_OBJECT) return nullptr;

    return &value->as_object;
}

/** Somebody said something in the channel. Answers `!ping` and logs the rest. */
static void on_chat(NYA_TwitchHelix* helix, NYA_ConstCString channel_id, NYA_ConstCString bot_id, const NYA_Object* event) {
    NYA_ConstCString who  = string_at(event, "chatter_user_name");
    NYA_ConstCString id   = string_at(event, "chatter_user_id");
    NYA_ConstCString text = string_at(object_at(event, "message"), "text");

    if (text == nullptr) return;

    // A bot that answers its own messages talks to itself forever, and Twitch delivers a bot's own chat back to it like anybody else's. The id is what tells them apart; the display name is not unique.
    if (id != nullptr && nya_string_equals(id, bot_id)) return;

    nya_log_info("<%s> %s", who != nullptr ? who : "someone", text);

    if (!nya_string_equals(text, PING_COMMAND)) return;

    NYA_Error queued = nya_twitch_helix_chat_send(helix, channel_id, "pong", nullptr);

    // Not fatal. A full queue means the bot is further behind than Twitch will let it catch up, and dropping one answer is the right thing to do about that.
    if (!queued.ok) nya_log_warn("Could not queue the answer: %s", (NYA_ConstCString)queued.message);
}

/* THE PROGRAM */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_unused(argv);

    nya_backtrace_init();
    defer nya_backtrace_deinit();

    NYA_ConstCString token      = getenv(TOKEN_VARIABLE);
    NYA_ConstCString client_id  = getenv(CLIENT_VARIABLE);
    NYA_ConstCString bot_id     = getenv(BOT_VARIABLE);
    NYA_ConstCString channel_id = getenv(CHANNEL_VARIABLE);

    if (token == nullptr || client_id == nullptr || bot_id == nullptr || channel_id == nullptr) {
        nya_log_info("This needs four things, and at least one of them is not set:");
        nya_log_info("  " TOKEN_VARIABLE "   a user token for the bot account, with user:read:chat and user:write:chat");
        nya_log_info("  " CLIENT_VARIABLE "   the application it was minted for");
        nya_log_info("  " BOT_VARIABLE "      the bot account's numeric user id");
        nya_log_info("  " CHANNEL_VARIABLE "  the numeric user id of the channel to listen to");
        nya_log_info("Register an application at https://dev.twitch.tv/console/apps to get the first two.");
        nya_log_info("Nothing was connected and nothing was sent.");

        return EXIT_SUCCESS;
    }

    (void)signal(SIGINT, stop);

    NYA_Arena* arena = nya_arena_create(.name = "twitch_bot");
    defer      nya_arena_destroy(arena);

    NYA_TwitchEventSub* events = nullptr;
    NYA_Error           opened = nya_twitch_eventsub_create(arena, (NYA_TwitchEventSubOptions){ 0 }, &events);

    if (!opened.ok) {
        nya_log_error("The socket would not start: %s", (NYA_ConstCString)opened.message);
        return EXIT_FAILURE;
    }
    defer nya_twitch_eventsub_destroy(events);

    NYA_TwitchHelix* helix = nullptr;
    NYA_Error        made  = nya_twitch_helix_create(arena,
                                                     (NYA_TwitchHelixOptions){
                                                        .token     = token,
                                                        .client_id = client_id,
                                                        .bot_id    = bot_id,
                                                     },
                                                     &helix);

    if (!made.ok) {
        nya_log_error("The API client would not start: %s", (NYA_ConstCString)made.message);
        return EXIT_FAILURE;
    }
    defer nya_twitch_helix_destroy(helix);

    nya_log_info("Connecting. Say %s in the channel; ctrl-c to stop.", PING_COMMAND);

    while (RUNNING) {
        NYA_TwitchEventSubMessage message = { 0 };

        while (nya_twitch_eventsub_poll(events, &message)) {
            switch (message.kind) {
                /* A session, which is the only thing a subscription can be made against — and a different one every time, so this runs again after every reconnect. A bot that subscribed once at start-up would go quiet the first time Twitch moved it. */
                case NYA_TWITCH_EVENTSUB_WELCOME: {
                    NYA_Error queued = nya_twitch_helix_subscribe(helix, CHAT_SUBSCRIPTION, CHAT_VERSION, channel_id, message.session, nullptr);

                    if (queued.ok) {
                        nya_log_info("Session %s; subscribing to %s.", message.session, CHAT_SUBSCRIPTION);
                    } else {
                        nya_log_error("Could not subscribe: %s", (NYA_ConstCString)queued.message);
                        RUNNING = 0;
                    }
                    break;
                }

                case NYA_TWITCH_EVENTSUB_NOTIFICATION: {
                    if (nya_string_equals(message.subscription_type, CHAT_SUBSCRIPTION)) {
                        on_chat(helix, channel_id, bot_id, message.event);
                    } else {
                        nya_log_debug("%s", message.subscription_type);
                    }
                    break;
                }

                /* Twitch dropped the subscription: the token was revoked, its scopes were taken away, or the channel banned the bot. None of those get better by subscribing again. */
                case NYA_TWITCH_EVENTSUB_REVOKED: {
                    nya_log_error("Twitch revoked %s; there is nothing left to listen to.", message.subscription_type);
                    RUNNING = 0;
                    break;
                }

                case NYA_TWITCH_EVENTSUB_DISCONNECTED: {
                    nya_log_warn("Disconnected: %s. Back in " FMTu64 " ms, and the new session will be subscribed again.", message.reason,
                                 message.retry_in_ms);
                    break;
                }

                case NYA_TWITCH_EVENTSUB_FATAL: {
                    nya_log_error("The socket cannot be opened: %s", message.reason);
                    RUNNING = 0;
                    break;
                }

                case NYA_TWITCH_EVENTSUB_NONE:
                case NYA_TWITCH_EVENTSUB_KIND_COUNT:
                default:                             break;
            }
        }

        NYA_TwitchHelixResult result = { 0 };

        while (nya_twitch_helix_poll(helix, &result)) {
            if (result.error.ok) {
                nya_log_debug("%s answered %u", result.route, result.status);
            } else {
                nya_log_warn("%s answered %u: %s", result.route, result.status, (NYA_ConstCString)result.error.message);
            }
        }

        // A real sleep, as the discord bot and the web server examples do, so the loop does not spin a core waiting for a message that may be an hour away.
        SDL_Delay(TICK_SLEEP_MS);
    }

    NYA_TwitchHelixLimit limit = nya_twitch_helix_limit(helix);

    nya_log_info("Stopping. The socket was %s, with %u calls still queued and %u of %u left in the rate limit.",
                 nya_twitch_eventsub_state_name(nya_twitch_eventsub_state(events)), nya_twitch_helix_pending(helix), limit.remaining, limit.limit);

    return EXIT_SUCCESS;
}
