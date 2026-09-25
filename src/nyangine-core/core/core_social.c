#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Steam's rich presence key that carries a joinable address. The name is Steam's, not this engine's:
 * the client shows "Join game" on a friend's list exactly when this key is set.
 * */
#define _NYA_SOCIAL_STEAM_KEY_CONNECT "connect"

/** The key Steam looks up in the app's localization tokens to word the friend list line. */
#define _NYA_SOCIAL_STEAM_KEY_DISPLAY "steam_display"

/** The two substitutions that token may use. Both are set whenever there is something to put in them. */
#define _NYA_SOCIAL_STEAM_KEY_DETAILS "details"
#define _NYA_SOCIAL_STEAM_KEY_STATE   "state"

/** One event's strings, kept alive until this slot comes round again. */
typedef struct {
    char user_id[NYA_SOCIAL_MAX_USER_ID];
    char user_name[NYA_SOCIAL_MAX_TEXT];
    char secret[NYA_SOCIAL_MAX_SECRET];
} _NYA_SocialPending;

typedef struct {
    b8 running;

    NYA_SocialConfig config;

    /** Whether each provider has ever been reported as up, so the "it is on" line is written once. */
    b8 discord_announced;
    b8 steam_announced;

    /**
     * The presence as the game last described it, with every string copied.
     * */
    b8   has_presence;
    char details[NYA_SOCIAL_MAX_TEXT];
    char state[NYA_SOCIAL_MAX_TEXT];
    char party_id[NYA_SOCIAL_MAX_TEXT];
    char secret[NYA_SOCIAL_MAX_SECRET];
    s64  start_time_s;
    u32  party_size;
    u32  party_max;

    /** What was last pushed to Steam, so an unchanged presence is not rewritten every frame. */
    char steam_details[NYA_SOCIAL_MAX_TEXT];
    char steam_state[NYA_SOCIAL_MAX_TEXT];
    char steam_secret[NYA_SOCIAL_MAX_SECRET];
    b8   steam_presence_sent;

    /** What was last written into the Steam lobby's own table, for the same reason. */
    char lobby_secret[NYA_SOCIAL_MAX_SECRET];

    /** Set by nya_social_invite_open while a lobby is being created for it. */
    b8 wants_invite_overlay;

    /** A lobby this module joined on the player's behalf, waiting for its join key to arrive. */
    NYA_SteamId pending_lobby;

    _NYA_SocialPending pending[NYA_SOCIAL_MAX_PENDING];
    u32                pending_next;
} _NYA_SocialSystem;

NYA_INTERNAL _NYA_SocialSystem _NYA_SOCIAL = { 0 };

/** The frame hook. Named, so the callback registry can re-resolve it after a code reload. */
NYA_INTERNAL void _nya_social_on_frame(NYA_Event* event);

/** Takes the next storage slot and copies `user_id`, `user_name` and `secret` into it. */
NYA_INTERNAL const _NYA_SocialPending* _nya_social_pending_take(NYA_ConstCString user_id, NYA_ConstCString user_name, NYA_ConstCString secret) __attr_no_discard;

/** Dispatches one social event with its strings copied into module storage. */
NYA_INTERNAL void _nya_social_dispatch(NYA_EventType type, NYA_SocialProvider provider, NYA_ConstCString user_id, NYA_ConstCString user_name,
                                       NYA_ConstCString secret);

/** Drains Discord's inbound queue. Does nothing in a build without the plugin. */
NYA_INTERNAL void _nya_social_pump_discord(void);

/** Drains Steam's lobby and invite queue. */
NYA_INTERNAL void _nya_social_pump_steam(void);

/** Pushes the held presence to Discord. */
NYA_INTERNAL void _nya_social_push_discord(void);

/** Pushes the held presence to Steam, as rich presence and as lobby data. */
NYA_INTERNAL void _nya_social_push_steam(void);

/** snprintf into a fixed buffer, with null meaning an empty string. */
NYA_INTERNAL void _nya_social_copy(OUT char* out, u64 capacity, NYA_ConstCString text);

/** Whether a buffer holds anything. */
NYA_INTERNAL b8 _nya_social_is_empty(NYA_ConstCString text) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_social_init_with_config(NYA_SocialConfig config) {
    if (_NYA_SOCIAL.running) return nya_error(NYA_ERROR_NOT_OK, "the social module is already initialized");

    _NYA_SOCIAL = (_NYA_SocialSystem){ .running = true, .config = config };

#ifdef NYA_PLUGIN_DISCORD
    if (config.discord_application_id != 0) {
        NYA_Error started = nya_discord_init(config.discord_application_id);

        // not fatal and not propagated: a refused application id leaves Discord off, and the game is
        // playable without it. The message is the whole report.
        if (!started.ok) nya_log_warn("Discord presence is off: %s", (NYA_ConstCString)started.message);
    }
#else
    if (config.discord_application_id != 0) nya_log_info("This build has no Discord plugin; rich presence and Discord invites are off.");
#endif

    // the pump runs at the top of every frame, before the game's own update, so an accepted invite is
    // in the event queue by the time the game drains it in the same frame.
    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_FRAME_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_social_on_frame),
    });

    return NYA_OK;
}

void nya_social_deinit(void) {
    if (!_NYA_SOCIAL.running) return;

    nya_event_hook_unregister((NYA_EventHook){
        .event_type = NYA_EVENT_FRAME_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_social_on_frame),
    });

    // before the providers go down, so a friend's list stops showing this player as in a game rather
    // than waiting for the process to exit.
    (void)nya_social_presence_clear();

#ifdef NYA_PLUGIN_DISCORD
    nya_discord_deinit();
#endif

    // Steam itself is brought down by the app, which owns the connection; only what this module put
    // on it is taken back.
    nya_steam_rich_presence_clear();
    nya_steam_lobby_leave();

    _NYA_SOCIAL = (_NYA_SocialSystem){ 0 };
}

b8 nya_social_available(void) {
    if (!_NYA_SOCIAL.running) return false;

#ifdef NYA_PLUGIN_DISCORD
    if (nya_discord_connected()) return true;
#endif

    return nya_steam_is_connected();
}

NYA_Error nya_social_presence_set(NYA_SocialPresence presence) {
    if (!_NYA_SOCIAL.running) return nya_error(NYA_ERROR_NOT_OK, "the social module is not initialized");

    // both or neither, here rather than at each provider: a size without a maximum means nothing to
    // either of them, and letting it through would produce two different wrong cards.
    if (presence.party_size > 0 && presence.party_max < presence.party_size) {
        presence.party_size = 0;
        presence.party_max  = 0;
    }

    _nya_social_copy(_NYA_SOCIAL.details, sizeof(_NYA_SOCIAL.details), presence.details);
    _nya_social_copy(_NYA_SOCIAL.state, sizeof(_NYA_SOCIAL.state), presence.state);
    _nya_social_copy(_NYA_SOCIAL.party_id, sizeof(_NYA_SOCIAL.party_id), presence.party_id);
    _nya_social_copy(_NYA_SOCIAL.secret, sizeof(_NYA_SOCIAL.secret), presence.join_secret);

    _NYA_SOCIAL.start_time_s = presence.start_time_s;
    _NYA_SOCIAL.party_size   = presence.party_size;
    _NYA_SOCIAL.party_max    = presence.party_max;
    _NYA_SOCIAL.has_presence = true;

    // pushed from the pump rather than here: both providers rate limit, and a game calling this every
    // frame must cost nothing more than the copies above.
    return NYA_OK;
}

NYA_Error nya_social_presence_clear(void) {
    if (!_NYA_SOCIAL.running) return nya_error(NYA_ERROR_NOT_OK, "the social module is not initialized");

    _NYA_SOCIAL.has_presence        = false;
    _NYA_SOCIAL.details[0]          = '\0';
    _NYA_SOCIAL.state[0]            = '\0';
    _NYA_SOCIAL.party_id[0]         = '\0';
    _NYA_SOCIAL.secret[0]           = '\0';
    _NYA_SOCIAL.start_time_s        = 0;
    _NYA_SOCIAL.party_size          = 0;
    _NYA_SOCIAL.party_max           = 0;
    _NYA_SOCIAL.steam_presence_sent = false;
    _NYA_SOCIAL.lobby_secret[0]     = '\0';

#ifdef NYA_PLUGIN_DISCORD
    if (nya_discord_status() != NYA_DISCORD_STATUS_OFF) (void)nya_discord_activity_clear();
#endif

    nya_steam_rich_presence_clear();

    return NYA_OK;
}

NYA_Error nya_social_join_reply(NYA_SocialProvider provider, NYA_ConstCString user_id, b8 accept) {
    if (!_NYA_SOCIAL.running) return nya_error(NYA_ERROR_NOT_OK, "the social module is not initialized");
    if (provider <= NYA_SOCIAL_PROVIDER_NONE || provider >= NYA_SOCIAL_PROVIDER_COUNT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no such social provider");
    }

    if (provider == NYA_SOCIAL_PROVIDER_DISCORD) {
#ifdef NYA_PLUGIN_DISCORD
        return nya_discord_join_reply(user_id, accept);
#else
        nya_unused(user_id, accept);
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no Discord plugin");
#endif
    }

    // Steam has no ask-to-join: an invite is accepted or ignored on the asking side, and there is
    // nothing for the host to answer. Reported rather than silently succeeding, so a game wiring a
    // decline button to it learns that the button does nothing there.
    nya_unused(user_id, accept);

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "Steam invites are answered by the person who was invited");
}

NYA_Error nya_social_invite_open(void) {
    if (!_NYA_SOCIAL.running) return nya_error(NYA_ERROR_NOT_OK, "the social module is not initialized");

    if (!nya_steam_is_connected()) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "no Steam client; a Discord invite is sent from the chat window");
    }

    if (nya_steam_id_is_set(nya_steam_lobby_current())) return nya_steam_lobby_invite_open();

    // no lobby yet, so one is created and the dialog opens when it exists. Friends-only rather than
    // public: this is the "invite somebody" button, not a server browser listing.
    NYA_TRY(nya_steam_lobby_create(NYA_STEAM_LOBBY_FRIENDS_ONLY, NYA_NET_MAX_PEERS));

    _NYA_SOCIAL.wants_invite_overlay = true;

    return NYA_OK;
}

NYA_ConstCString nya_social_user_name(void) {
    if (!_NYA_SOCIAL.running) return "";

#ifdef NYA_PLUGIN_DISCORD
    NYA_ConstCString discord = nya_discord_user_name();
    if (discord != nullptr) return discord;
#endif

    return nya_steam_user_name();
}

void nya_social_pump(void) {
    if (!_NYA_SOCIAL.running) return;

#ifdef NYA_PLUGIN_DISCORD
    nya_discord_pump();

    if (!_NYA_SOCIAL.discord_announced && nya_discord_connected()) {
        _NYA_SOCIAL.discord_announced = true;
        nya_log_info("Discord rich presence is on, signed in as '%s'.", nya_discord_user_name() == nullptr ? "?" : nya_discord_user_name());
    }
#endif

    if (!_NYA_SOCIAL.steam_announced && nya_steam_is_connected()) {
        _NYA_SOCIAL.steam_announced = true;
        nya_log_info("Steam friends and invites are on, signed in as '%s'.", nya_steam_user_name());
    }

    _nya_social_pump_discord();
    _nya_social_pump_steam();

    _nya_social_push_discord();
    _nya_social_push_steam();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_social_on_frame(NYA_Event* event) {
    nya_unused(event);

    nya_social_pump();
}

void _nya_social_copy(OUT char* out, u64 capacity, NYA_ConstCString text) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    // truncated rather than refused: presence is cosmetic, and a long level name should shorten rather
    // than take the card down. A secret is the exception and is checked by its own parser before use.
    (void)snprintf(out, capacity, "%s", text == nullptr ? "" : text);
}

b8 _nya_social_is_empty(NYA_ConstCString text) {
    return text == nullptr || text[0] == '\0';
}

const _NYA_SocialPending* _nya_social_pending_take(NYA_ConstCString user_id, NYA_ConstCString user_name, NYA_ConstCString secret) {
    nya_assert(_NYA_SOCIAL.pending_next < NYA_SOCIAL_MAX_PENDING);

    _NYA_SocialPending* slot = &_NYA_SOCIAL.pending[_NYA_SOCIAL.pending_next];

    // round robin rather than "the next free one": nothing here knows when a handler is done with a
    // slot, and going round gives every event the longest life the fixed storage can offer.
    _NYA_SOCIAL.pending_next = (_NYA_SOCIAL.pending_next + 1) % NYA_SOCIAL_MAX_PENDING;

    _nya_social_copy(slot->user_id, sizeof(slot->user_id), user_id);
    _nya_social_copy(slot->user_name, sizeof(slot->user_name), user_name);
    _nya_social_copy(slot->secret, sizeof(slot->secret), secret);

    return slot;
}

void _nya_social_dispatch(NYA_EventType type, NYA_SocialProvider provider, NYA_ConstCString user_id, NYA_ConstCString user_name, NYA_ConstCString secret) {
    nya_assert(type == NYA_EVENT_SOCIAL_JOIN || type == NYA_EVENT_SOCIAL_JOIN_REQUEST);
    nya_assert(provider > NYA_SOCIAL_PROVIDER_NONE && provider < NYA_SOCIAL_PROVIDER_COUNT);

    const _NYA_SocialPending* slot = _nya_social_pending_take(user_id, user_name, secret);

    nya_event_dispatch((NYA_Event){
        .type = type,
        .as_social_event = {
            .provider  = provider,
            .user_id   = slot->user_id[0] == '\0' ? nullptr : slot->user_id,
            .user_name = slot->user_name[0] == '\0' ? nullptr : slot->user_name,
            .secret    = slot->secret[0] == '\0' ? nullptr : slot->secret,
        },
    });
}

void _nya_social_pump_discord(void) {
#ifdef NYA_PLUGIN_DISCORD
    NYA_DiscordEvent event;

    while (nya_discord_poll(&event)) {
        switch (event.kind) {
            case NYA_DISCORD_EVENT_JOIN:
                _nya_social_dispatch(NYA_EVENT_SOCIAL_JOIN, NYA_SOCIAL_PROVIDER_DISCORD, nullptr, nullptr, event.secret);
                break;

            case NYA_DISCORD_EVENT_JOIN_REQUEST:
                _nya_social_dispatch(NYA_EVENT_SOCIAL_JOIN_REQUEST, NYA_SOCIAL_PROVIDER_DISCORD, event.user_id, event.user_name, nullptr);
                break;

            case NYA_DISCORD_EVENT_NONE:
            case NYA_DISCORD_EVENT_KIND_COUNT:
            default: nya_unreachable();
        }
    }
#endif
}

void _nya_social_pump_steam(void) {
    NYA_SteamEvent event;

    while (nya_steam_poll(&event)) {
        switch (event.kind) {
            case NYA_STEAM_EVENT_JOIN_REQUESTED: {
                /*
                 * Two shapes of invite, and only one of them arrives with an address in it.
                 */
                // a rich presence "connect" string is the address itself, so it goes straight out.
                if (!_nya_social_is_empty(event.secret)) {
                    _nya_social_dispatch(NYA_EVENT_SOCIAL_JOIN, NYA_SOCIAL_PROVIDER_STEAM, nullptr, nya_steam_friend_name(event.user), event.secret);
                    break;
                }

                // a lobby invite carries only the lobby, so this module joins it and waits for its
                // data; the address is published in the lobby's own table by the host's presence.
                if (!nya_steam_id_is_set(event.lobby)) break;

                NYA_Error joining = nya_steam_lobby_join(event.lobby);

                if (!joining.ok) {
                    nya_log_warn("Could not join the Steam lobby a friend invited to: %s", (NYA_ConstCString)joining.message);
                    break;
                }

                _NYA_SOCIAL.pending_lobby = event.lobby;
            } break;

            case NYA_STEAM_EVENT_LOBBY_ENTERED:
            case NYA_STEAM_EVENT_LOBBY_DATA_CHANGED: {
                // the overlay was asked for before the lobby existed; now it does.
                if (_NYA_SOCIAL.wants_invite_overlay && event.kind == NYA_STEAM_EVENT_LOBBY_ENTERED) {
                    _NYA_SOCIAL.wants_invite_overlay = false;

                    NYA_Error opened = nya_steam_lobby_invite_open();
                    if (!opened.ok) nya_log_warn("Could not open the Steam invite dialog: %s", (NYA_ConstCString)opened.message);
                }

                if (!nya_steam_id_is_set(_NYA_SOCIAL.pending_lobby)) break;
                if (!nya_steam_id_equals(event.lobby, _NYA_SOCIAL.pending_lobby)) break;

                NYA_ConstCString secret = nya_steam_lobby_data_get(_NYA_SOCIAL.pending_lobby, NYA_SOCIAL_LOBBY_KEY_JOIN);

                // still empty: the host has not published it yet, and the next LOBBY_DATA_CHANGED for
                // this lobby will bring it.
                if (_nya_social_is_empty(secret)) break;

                _NYA_SOCIAL.pending_lobby = NYA_STEAM_ID_NONE;

                _nya_social_dispatch(NYA_EVENT_SOCIAL_JOIN, NYA_SOCIAL_PROVIDER_STEAM, nullptr, nullptr, secret);
            } break;

            case NYA_STEAM_EVENT_LOBBY_FAILED: {
                if (nya_steam_id_is_set(_NYA_SOCIAL.pending_lobby)) {
                    nya_log_warn("A Steam lobby invite could not be taken up (result %u).", event.reason);
                    _NYA_SOCIAL.pending_lobby = NYA_STEAM_ID_NONE;
                }

                _NYA_SOCIAL.wants_invite_overlay = false;
            } break;

            // the game's own business, and nothing this facade has to translate.
            case NYA_STEAM_EVENT_LOBBY_MEMBER_CHANGED:
            case NYA_STEAM_EVENT_LOBBY_LIST:          break;

            // routed to nya_steam_p2p_poll, so they can never reach this queue.
            case NYA_STEAM_EVENT_SESSION_REQUEST:
            case NYA_STEAM_EVENT_SESSION_FAILED:
            case NYA_STEAM_EVENT_NONE:
            case NYA_STEAM_EVENT_KIND_COUNT:
            default: nya_unreachable();
        }
    }
}

void _nya_social_push_discord(void) {
#ifdef NYA_PLUGIN_DISCORD
    if (!nya_discord_connected()) return;
    if (!_NYA_SOCIAL.has_presence) return;

    // the plugin compares the activity with what it last sent and writes nothing when they match, so
    // handing it the same struct every frame costs one comparison.
    (void)nya_discord_activity_set((NYA_DiscordActivity){
        .details      = _NYA_SOCIAL.details[0] == '\0' ? nullptr : _NYA_SOCIAL.details,
        .state        = _NYA_SOCIAL.state[0] == '\0' ? nullptr : _NYA_SOCIAL.state,
        .start_time_s = _NYA_SOCIAL.start_time_s,
        .large_image  = _NYA_SOCIAL.config.large_image,
        .large_text   = _NYA_SOCIAL.config.large_text,
        .party_id     = _NYA_SOCIAL.party_id[0] == '\0' ? nullptr : _NYA_SOCIAL.party_id,
        .party_size   = _NYA_SOCIAL.party_size,
        .party_max    = _NYA_SOCIAL.party_max,
        .join_secret  = _NYA_SOCIAL.secret[0] == '\0' ? nullptr : _NYA_SOCIAL.secret,
    });
#endif
}

void _nya_social_push_steam(void) {
    if (!nya_steam_is_connected()) return;
    if (!_NYA_SOCIAL.has_presence) return;

    b8 changed = !_NYA_SOCIAL.steam_presence_sent || !nya_string_equals(_NYA_SOCIAL.steam_details, _NYA_SOCIAL.details)
              || !nya_string_equals(_NYA_SOCIAL.steam_state, _NYA_SOCIAL.state) || !nya_string_equals(_NYA_SOCIAL.steam_secret, _NYA_SOCIAL.secret);

    if (changed) {
        (void)nya_steam_rich_presence_set(_NYA_SOCIAL_STEAM_KEY_DETAILS, _NYA_SOCIAL.details);
        (void)nya_steam_rich_presence_set(_NYA_SOCIAL_STEAM_KEY_STATE, _NYA_SOCIAL.state);

        // the token the partner site's localization file resolves to a sentence. "#Status" is the
        // conventional name for the one that substitutes %details% and %state%.
        (void)nya_steam_rich_presence_set(_NYA_SOCIAL_STEAM_KEY_DISPLAY, "#Status");

        // "connect" is what makes "Join game" appear on a friend's list, and clearing it is what makes
        // it disappear again when a session stops being joinable.
        (void)nya_steam_rich_presence_set(_NYA_SOCIAL_STEAM_KEY_CONNECT, _NYA_SOCIAL.secret);

        _nya_social_copy(_NYA_SOCIAL.steam_details, sizeof(_NYA_SOCIAL.steam_details), _NYA_SOCIAL.details);
        _nya_social_copy(_NYA_SOCIAL.steam_state, sizeof(_NYA_SOCIAL.steam_state), _NYA_SOCIAL.state);
        _nya_social_copy(_NYA_SOCIAL.steam_secret, sizeof(_NYA_SOCIAL.steam_secret), _NYA_SOCIAL.secret);

        _NYA_SOCIAL.steam_presence_sent = true;
    }

    /*
     * And the lobby's own table, which is what a lobby invite carries in place of an address.
     */
    NYA_SteamId lobby = nya_steam_lobby_current();

    if (!nya_steam_id_is_set(lobby)) return;
    if (!nya_steam_id_equals(nya_steam_lobby_owner(lobby), nya_steam_user_id())) return;
    if (nya_string_equals(_NYA_SOCIAL.lobby_secret, _NYA_SOCIAL.secret)) return;

    NYA_Error written = nya_steam_lobby_data_set(NYA_SOCIAL_LOBBY_KEY_JOIN, _NYA_SOCIAL.secret);

    if (!written.ok) {
        nya_log_warn("Could not publish the join address in the Steam lobby: %s", (NYA_ConstCString)written.message);
        return;
    }

    _nya_social_copy(_NYA_SOCIAL.lobby_secret, sizeof(_NYA_SOCIAL.lobby_secret), _NYA_SOCIAL.secret);
}
