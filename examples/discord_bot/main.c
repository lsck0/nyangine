/**
 * @file examples/discord_bot/main.c
 *
 * A Discord bot: no window, no renderer, no world. A websocket held open against Discord's gateway, the
 * events that come down it, and the HTTP calls that answer them.
 *
 * ```
 * ./build run example discord_bot                        # says there is no token and stops
 * DISCORD_BOT_TOKEN=... ./discord_bot.example            # connects and listens
 * DISCORD_BOT_TOKEN=... DISCORD_APPLICATION_ID=... ./discord_bot.example --register
 * ```
 *
 * It answers two things: `!ping` in any channel it can read, and the `/ping` slash command once that has
 * been registered with `--register`. Everything else it sees, it logs.
 *
 * ## Running it with no token
 *
 * This is what CI does. A bot with no credentials is not a failure — there is nothing wrong with the
 * build — so it says what is missing, says where to get it, and exits zero. The same program with the
 * variable set is the real thing; nothing else changes.
 *
 * ## Getting a token
 *
 * An application at `https://discord.com/developers/applications`, a bot user under it, and the token
 * from that page. `MESSAGE_CONTENT` is a privileged intent and has to be switched on there as well, or
 * every message arrives with an empty `content` and the bot looks broken. Put the token in the
 * environment or a file outside the repository; never in a source file, and never in a commit.
 *
 * ## The two clients
 *
 * The gateway and the REST client are separate on purpose and are polled separately. The gateway is the
 * long lived socket that tells the bot what happened; the REST client is the queue of things the bot
 * wants to do about it, and it is the one that knows about rate limits. Neither blocks the loop below
 * for longer than one HTTP transfer, which is why this shape drops straight into a game's frame.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/* CONSTANTS */

/** Where the token is read from. An environment variable, so nothing of it is in this tree. */
#define TOKEN_VARIABLE "DISCORD_BOT_TOKEN"

/** The application the slash command is registered under. Only `--register` needs it. */
#define APPLICATION_VARIABLE "DISCORD_APPLICATION_ID"

/** How long the loop sleeps between polls. Nothing here is on a frame budget, so this is generous. */
#define TICK_SLEEP_MS 20

/** The one message command, answered wherever the bot can read it. */
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

/**
 * A message in a channel the bot can see. Answers `!ping` and logs the rest.
 * */
static void on_message(NYA_DiscordRest* rest, const NYA_Object* message) {
    NYA_ConstCString content    = string_at(message, "content");
    NYA_ConstCString channel_id = string_at(message, "channel_id");
    NYA_ConstCString author     = string_at(object_at(message, "author"), "username");

    if (content == nullptr || channel_id == nullptr) return;

    // A bot that answers its own messages talks to itself forever. Discord marks its own kind, and this is the cheapest of the several ways to check.
    NYA_Value* is_bot = nya_object_get(object_at(message, "author"), "bot");
    if (is_bot != nullptr && is_bot->type == NYA_TYPE_B8 && is_bot->as_b8) return;

    nya_log_info("<%s> %s", author != nullptr ? author : "someone", content);

    if (!nya_string_equals(content, PING_COMMAND)) return;

    u64       id     = 0;
    NYA_Error queued = nya_discord_rest_message_send(rest, channel_id, "pong", &id);

    // Not fatal. A full queue means the bot is further behind than Discord will let it catch up, and dropping one answer is the right thing to do about that.
    if (!queued.ok) nya_log_warn("Could not queue the answer: %s", (NYA_ConstCString)queued.message);
}

/**
 * A slash command. Discord gives three seconds to answer one, so the reply is queued before anything
 * else this frame.
 * */
static void on_interaction(NYA_DiscordRest* rest, const NYA_Object* interaction) {
    NYA_ConstCString id    = string_at(interaction, "id");
    NYA_ConstCString token = string_at(interaction, "token");
    NYA_ConstCString name  = string_at(object_at(interaction, "data"), "name");

    if (id == nullptr || token == nullptr || name == nullptr) return;

    nya_log_info("/%s", name);

    if (!nya_string_equals(name, "ping")) return;

    u64       queued_id = 0;
    NYA_Error queued    = nya_discord_rest_interaction_reply(rest, id, token, "pong", &queued_id);

    if (!queued.ok) nya_log_warn("Could not queue the reply: %s", (NYA_ConstCString)queued.message);
}

/* THE PROGRAM */

s32 main(s32 argc, char** argv) {
    nya_log_level_set(NYA_LOG_LEVEL_INFO);

    b8 register_commands = false;
    for (s32 i = 1; i < argc; i++) register_commands |= nya_string_equals(argv[i], "--register");

    NYA_ConstCString token = getenv(TOKEN_VARIABLE);

    /* The CI path. There is nothing wrong with a build that has no Discord credentials in it, so this says what is missing and leaves successfully rather than failing a pipeline over a secret that is deliberately not in the repository. */
    if (token == nullptr || token[0] == '\0') {
        nya_log_info("No " TOKEN_VARIABLE " is set, so there is nothing to log in as.");
        nya_log_info("Make an application at https://discord.com/developers/applications, add a bot user,");
        nya_log_info("and run this as: " TOKEN_VARIABLE "=<the token> ./discord_bot.example");
        nya_log_info("Nothing was connected and nothing was sent.");

        return EXIT_SUCCESS;
    }

    (void)signal(SIGINT, stop);

    NYA_Arena* arena = nya_arena_create(.name = "discord_bot");
    defer      nya_arena_destroy(arena);

    /* The gateway. MESSAGE_CONTENT is privileged and has to be enabled on the application's page; asking for it without that is close code 4014, which this client treats as fatal rather than retrying into a disabled token. */
    NYA_DiscordGateway* gateway = nullptr;
    NYA_Error           opened  = nya_discord_gateway_create(
        arena,
        (NYA_DiscordGatewayOptions){
                     .token   = token,
                     .intents = NYA_DISCORD_INTENT_GUILDS | NYA_DISCORD_INTENT_GUILD_MESSAGES | NYA_DISCORD_INTENT_MESSAGE_CONTENT,
        },
        &gateway
    );

    if (!opened.ok) {
        nya_log_error("The gateway would not start: %s", (NYA_ConstCString)opened.message);
        return EXIT_FAILURE;
    }
    defer nya_discord_gateway_destroy(gateway);

    NYA_DiscordRest* rest      = nullptr;
    NYA_Error        rest_made = nya_discord_rest_create(
        arena,
        (NYA_DiscordRestOptions){
                   .token          = token,
                   .application_id = getenv(APPLICATION_VARIABLE),
        },
        &rest
    );

    if (!rest_made.ok) {
        nya_log_error("The REST client would not start: %s", (NYA_ConstCString)rest_made.message);
        return EXIT_FAILURE;
    }
    defer nya_discord_rest_destroy(rest);

    if (register_commands) {
        u64       id       = 0;
        NYA_Error queued   = nya_discord_rest_command_register(rest, "ping", "Answer with pong", &id);

        // A global command takes up to an hour to appear everywhere, so this is a thing to run once and not on every start.
        if (queued.ok) {
            nya_log_info("Registering /ping. A global command can take an hour to appear in every server.");
        } else {
            nya_log_warn("Could not register /ping: %s", (NYA_ConstCString)queued.message);
        }
    }

    nya_log_info("Connecting. Say %s in a channel this bot can read; ctrl-c to stop.", PING_COMMAND);

    while (RUNNING) {
        /* The gateway first. Neither of these blocks the loop: the gateway never does at all, and the REST client only for the one transfer it decides may go. */
        NYA_DiscordGatewayEvent event = { 0 };

        while (nya_discord_gateway_poll(gateway, &event)) {
            switch (event.kind) {
                case NYA_DISCORD_GATEWAY_EVENT_READY: nya_log_info("Logged in as %s.", event.name); break;

                case NYA_DISCORD_GATEWAY_EVENT_RESUMED:
                    nya_log_info("Resumed at sequence " FMTs64 "; nothing was missed.", nya_discord_gateway_sequence(gateway));
                    break;

                case NYA_DISCORD_GATEWAY_EVENT_DISPATCH:
                    if (nya_string_equals(event.name, "MESSAGE_CREATE")) {
                        on_message(rest, event.data);
                    } else if (nya_string_equals(event.name, "INTERACTION_CREATE")) {
                        on_interaction(rest, event.data);
                    } else {
                        nya_log_debug("%s", event.name);
                    }
                    break;

                case NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED:
                    nya_log_warn("Disconnected (%u): %s. Back in " FMTu64 " ms.", event.code, event.reason, event.retry_in_ms);
                    break;

                /* The token, the shard or the intents. Reconnecting cannot fix any of them and doing it anyway is what gets a token disabled, so this stops. */
                case NYA_DISCORD_GATEWAY_EVENT_FATAL:
                    nya_log_error("Discord refused this bot (%u): %s", event.code, event.reason);
                    RUNNING = 0;
                    break;

                case NYA_DISCORD_GATEWAY_EVENT_NONE:
                case NYA_DISCORD_GATEWAY_EVENT_KIND_COUNT:
                default:                                   break;
            }
        }

        NYA_DiscordRestResult result = { 0 };

        while (nya_discord_rest_poll(rest, &result)) {
            if (result.error.ok) {
                nya_log_debug("%s answered %u", result.route, result.status);
            } else {
                nya_log_warn("%s answered %u: %s", result.route, result.status, (NYA_ConstCString)result.error.message);
            }
        }

        // A real sleep, as the web server example and the frame limiter do, so the loop does not spin a core waiting for a message that may be an hour away.
        SDL_Delay(TICK_SLEEP_MS);
    }

    nya_log_info("Stopping. The gateway was %s, with %u requests still queued.", nya_discord_gateway_state_name(nya_discord_gateway_state(gateway)),
                 nya_discord_rest_pending(rest));

    return EXIT_SUCCESS;
}
