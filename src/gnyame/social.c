/**
 * @file social.c
 *
 * What the game tells a friends service, and what it does with what comes back.
 *
 * The presence card is rebuilt from real state every tick and handed to nya_social_presence_set, which
 * compares it with what it last sent, so describing the whole session every tick costs a few string
 * copies. An accepted invite arrives as an engine event, is parsed into a launch config, and is applied
 * at the simulation barrier like any other screen change. A friend asking to join opens a prompt layer.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One join request waiting for the player to answer it, and the last presence that was published. */
typedef struct {
    b8 running;

    /** Unix seconds the process started, so a friend's list counts the whole session rather than a screen. */
    s64 started_s;

    b8                 request_pending;
    NYA_SocialProvider request_provider;
    char               request_user_id[NYA_SOCIAL_MAX_USER_ID];
    char               request_user_name[NYA_SOCIAL_MAX_TEXT];

    /** Rebuilt every tick and handed to the engine, which decides whether anything has to be sent. */
    char details[NYA_SOCIAL_MAX_TEXT];
    char state[NYA_SOCIAL_MAX_TEXT];
    char secret[NYA_SOCIAL_MAX_SECRET];
} _GNY_Social;

NYA_INTERNAL _GNY_Social _GNY_SOCIAL = { 0 };

/** Engine event hooks. Named, so the callback registry re-resolves them after a code reload. */
NYA_INTERNAL void _gny_social_on_join(NYA_Event* event);
NYA_INTERNAL void _gny_social_on_join_request(NYA_Event* event);

/** Applies a launch config a friend handed over, at the barrier. */
NYA_INTERNAL void _gny_social_join_apply(void* data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_social_start(void) {
    if (_GNY_SOCIAL.running) return;

    _GNY_SOCIAL = (_GNY_Social){ .running = true, .started_s = (s64)nya_clock_get_timestamp_s() };

    // never fails over a missing client, so the return is the caller's to log and nothing more.
    NYA_Error started = nya_social_init(
        .discord_application_id = GNY_DISCORD_APP_ID,
        .large_image            = GNY_DISCORD_LARGE_IMAGE,
        .large_text             = GNY_WINDOW_MAIN_TITLE
    );

    if (!started.ok) {
        nya_log_warn("Presence and invites are off: %s", (NYA_ConstCString)started.message);
        _GNY_SOCIAL.running = false;
        return;
    }

    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_SOCIAL_JOIN,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_gny_social_on_join),
    });

    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_SOCIAL_JOIN_REQUEST,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_gny_social_on_join_request),
    });
}

void gny_social_stop(void) {
    if (!_GNY_SOCIAL.running) return;

    nya_event_hook_unregister((NYA_EventHook){
        .event_type = NYA_EVENT_SOCIAL_JOIN,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_gny_social_on_join),
    });

    nya_event_hook_unregister((NYA_EventHook){
        .event_type = NYA_EVENT_SOCIAL_JOIN_REQUEST,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_gny_social_on_join_request),
    });

    nya_social_deinit();

    _GNY_SOCIAL = (_GNY_Social){ 0 };
}

void gny_social_update(f32 delta_time_s) {
    nya_unused(delta_time_s);

    if (!_GNY_SOCIAL.running) return;

    /*
     * What screen the player is on, from the layer stack rather than from a variable that could disagree
     * with it.
     */
    NYA_ConstCString details = nya_string_presence_menu();

    if (nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_CUBE3D_ID) != nullptr) details = nya_string_presence_3d();
    else if (nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_GAME_ID) != nullptr) details = nya_string_presence_sandbox();

    (void)snprintf(_GNY_SOCIAL.details, sizeof(_GNY_SOCIAL.details), "%s", details);

    /*
     * And what the session is: alone, hosting, or somebody else's.
     */
    u32 party_size = 0;
    u32 party_max  = 0;

    if (nya_net_client_state() == NYA_NET_CLIENT_PLAYING && !nya_net_server_running()) {
        (void)snprintf(_GNY_SOCIAL.state, sizeof(_GNY_SOCIAL.state), "%s", nya_string_presence_joined());
    } else if (nya_net_server_is_listening()) {
        party_size = nya_net_server_peer_count();
        party_max  = GNY_LAUNCH.max_players == 0 ? NYA_NET_MAX_PEERS : GNY_LAUNCH.max_players;

        (void)snprintf(_GNY_SOCIAL.state, sizeof(_GNY_SOCIAL.state), "%s", nya_string_presence_hosting());
    } else {
        (void)snprintf(_GNY_SOCIAL.state, sizeof(_GNY_SOCIAL.state), "%s", nya_string_presence_alone());
    }

    // only a listening server is joinable. a single player session has no port open, and a client is
    // not the authority, so inviting somebody to either would hand them an address nobody answers.
    _GNY_SOCIAL.secret[0] = '\0';

    if (nya_net_server_is_listening()) {
        NYA_NetLaunchConfig hosting = GNY_LAUNCH;

        // the server's real key, not the launch config's, which is empty when it made a throwaway one.
        const u8* key = nya_net_server_public_key();
        if (key != nullptr) nya_memcpy(hosting.server_key, key, NYA_NET_KEY_SIZE);

        (void)nya_net_config_to_join_secret(&hosting, _GNY_SOCIAL.secret, sizeof(_GNY_SOCIAL.secret));
    }

    (void)nya_social_presence_set((NYA_SocialPresence){
        .details      = _GNY_SOCIAL.details,
        .state        = _GNY_SOCIAL.state,
        .start_time_s = _GNY_SOCIAL.started_s,
        .party_id     = party_size > 0 ? GNY_WINDOW_MAIN_TITLE : nullptr,
        .party_size   = party_size,
        .party_max    = party_max,
        .join_secret  = _GNY_SOCIAL.secret[0] == '\0' ? nullptr : _GNY_SOCIAL.secret,
    });
}

b8 gny_social_request_pending(void) {
    return _GNY_SOCIAL.request_pending;
}

NYA_ConstCString gny_social_request_name(void) {
    if (!_GNY_SOCIAL.request_pending) return "";

    // the id when the provider gave no display name, so the prompt always names somebody.
    return _GNY_SOCIAL.request_user_name[0] != '\0' ? _GNY_SOCIAL.request_user_name : _GNY_SOCIAL.request_user_id;
}

void gny_social_request_answer(b8 accept) {
    if (!_GNY_SOCIAL.request_pending) return;

    NYA_Error answered = nya_social_join_reply(_GNY_SOCIAL.request_provider, _GNY_SOCIAL.request_user_id, accept);

    // logged and dropped: the person asking sees nothing happen, which is what a declined request looks
    // like anyway, and there is nothing useful to show the player about it.
    if (!answered.ok) nya_log_warn("Could not answer the join request: %s", (NYA_ConstCString)answered.message);

    _GNY_SOCIAL.request_pending = false;

    gny_screen_request(GNY_SCREEN_SOCIAL_DISMISS);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_social_on_join(NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_NetLaunchConfig joining = { 0 };

    // the secret came from another player's client through a third one, so it is parsed rather than
    // trusted, and a secret this build cannot read is a line in the log and nothing more.
    if (!nya_net_config_from_join_secret(event->as_social_event.secret, &joining)) {
        nya_log_warn("A friend's invite carried something this build cannot join.");
        return;
    }

    // at the barrier: joining despawns every entity the old session owned, and the layer stack is
    // mid-iteration here.
    nya_sim_defer(_gny_social_join_apply, &joining, sizeof(joining));

    event->was_handled = true;
}

void _gny_social_on_join_request(NYA_Event* event) {
    nya_assert(event != nullptr);

    const NYA_SocialEvent* social = &event->as_social_event;

    if (social->user_id == nullptr) return;

    // the newest request replaces an unanswered one rather than queueing: two prompts on screen is
    // worse than one, and the older request has usually expired by the time the player looks.
    _GNY_SOCIAL.request_provider = social->provider;

    (void)snprintf(_GNY_SOCIAL.request_user_id, sizeof(_GNY_SOCIAL.request_user_id), "%s", social->user_id);
    (void)snprintf(_GNY_SOCIAL.request_user_name, sizeof(_GNY_SOCIAL.request_user_name), "%s", social->user_name == nullptr ? "" : social->user_name);

    _GNY_SOCIAL.request_pending = true;

    gny_screen_request(GNY_SCREEN_SOCIAL_PROMPT);

    event->was_handled = true;
}

void _gny_social_join_apply(void* data) {
    NYA_NetLaunchConfig joining = *(NYA_NetLaunchConfig*)data;

    // the player's own name and their bad-network settings are theirs, not the inviter's.
    (void)snprintf(joining.name, sizeof(joining.name), "%s", GNY_LAUNCH.name);

    joining.conditions  = GNY_LAUNCH.conditions;
    joining.max_players = GNY_LAUNCH.max_players;
    joining.world_seed  = GNY_LAUNCH.world_seed;

    nya_log_info("Joining a friend's game over %s.", joining.transport == NYA_NET_TRANSPORT_STEAM ? "Steam" : "UDP");

    gny_net_rejoin(joining);

    // into the game if the player was still in a menu. Requested rather than applied: this already runs
    // at the barrier, and the screen change wants the next one.
    if (nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU_ID) != nullptr) gny_screen_request(GNY_SCREEN_START_GAME);
}
