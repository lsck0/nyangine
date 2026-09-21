/**
 * @file net.c
 *
 * Networking through one code path: single player is a server nobody joined. The same command function
 * moves the player on server and client, so prediction matches.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE GAME'S SIDE OF THE NETWORK
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Four modes, one path.
 */

/**
 * How a game action maps onto a command's bitfield.
 * */
#define GNY_COMMAND_BIT(action) ((u32)((action) - NYA_INPUT_ACTION_USER))

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_net_start(void) {
    f32 speed = NYA_CONFIG.game.player_speed > 0.0F ? NYA_CONFIG.game.player_speed : GNY_PLAYER_SPEED;

    if (GNY_LAUNCH.role == NYA_NET_ROLE_CLIENT) {
        NYA_NetClientConfig config = {
            .replicated_flag   = GNY_FLAG_REPLICATED,
            .on_apply_command  = nya_callback(gny_net_apply_command),
            .on_sample_command = nya_callback(gny_net_sample_command),
            .conditions        = GNY_LAUNCH.conditions,
        };

        nya_memcpy(config.server_key, GNY_LAUNCH.server_key, NYA_NET_KEY_SIZE);

        // anonymous rather than no game when the save root is unwritable.
        NYA_Error identified = nya_net_key_pair_load(GNY_NET_PLAYER_IDENTITY, &config.identity);
        if (!identified.ok) nya_log_warn("Connecting anonymously: %s", (NYA_ConstCString)identified.message);

        NYA_Error connected = nya_net_client_connect_on(GNY_LAUNCH.transport, GNY_LAUNCH.address, GNY_LAUNCH.port, GNY_LAUNCH.name, config);

        /*
         * A failed connection is not a crash.
         */
        if (!connected.ok) {
            nya_log_error("Could not reach %s:%u (%s); starting single player instead.", GNY_LAUNCH.address, GNY_LAUNCH.port,
                          (NYA_ConstCString)connected.message);

            GNY_LAUNCH.role = NYA_NET_ROLE_SERVER;
        } else {
            return;
        }
    }

    NYA_NetServerConfig server = {
        .replicated_flag  = GNY_FLAG_REPLICATED,
        .max_players      = GNY_LAUNCH.max_players,
        .on_spawn_player  = nya_callback(gny_net_spawn_player),
        .on_apply_command = nya_callback(gny_net_apply_command),
        .max_speed        = speed * GNY_NET_SPEED_HEADROOM,
        .position_bits    = GNY_NET_POSITION_BITS,
        .conditions       = GNY_LAUNCH.conditions,
    };

    // a listening server keeps its identity, so players who pinned its key can come back. Without one it makes a throwaway.
    if (GNY_LAUNCH.listen_port != 0) {
        NYA_Error identified = nya_net_key_pair_load(GNY_NET_SERVER_IDENTITY, &server.identity);
        if (!identified.ok) nya_log_warn("Using a throwaway server key: %s", (NYA_ConstCString)identified.message);
    }

    NYA_EXPECT(nya_net_server_start(server), "while starting the server");

    // Single player is this same server with nothing after it. That is the whole architecture; see net.h.
    if (GNY_LAUNCH.listen_port != 0) {
        NYA_Error listening = nya_net_server_listen_on(GNY_LAUNCH.transport, GNY_LAUNCH.listen_port);

        if (!listening.ok) {
            /*
             * A dedicated server that cannot bind has no reason to exist, so that one is fatal. A
             * listen server that cannot bind is still a perfectly good single player game, so that one
             * is not.
             */
            if (GNY_LAUNCH.dedicated) NYA_EXPECT(listening, "a dedicated server could not open its port");

            nya_log_error("Could not open port %u (%s); playing single player.", GNY_LAUNCH.listen_port, (NYA_ConstCString)listening.message);
        }
    }

    // A dedicated server has no local player, and that is the only thing that distinguishes it from a
    // listen server here.
    if (GNY_LAUNCH.dedicated) return;

    NYA_NetTransport* local = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&local), "while attaching the local player");

    NYA_EXPECT(nya_net_client_attach(local, GNY_LAUNCH.name, (NYA_NetClientConfig){
        .replicated_flag   = GNY_FLAG_REPLICATED,
        .on_apply_command  = nya_callback(gny_net_apply_command),
        .on_sample_command = nya_callback(gny_net_sample_command),
    }), "while attaching the local client");
}

void gny_net_stop(void) {
    nya_net_client_disconnect();
    nya_net_server_stop();
}

void gny_net_rejoin(NYA_NetLaunchConfig config) {
    // every player entity the old session spawned goes with it, replicated or local, or the new one
    // would replicate onto handles that already belong to somebody.
    gny_net_stop();

    GNY_LAUNCH = config;

    // the same path a cold start takes, including its fallback: a friend whose game has already ended
    // leaves this player in single player rather than in nothing at all.
    gny_net_start();
}

void gny_net_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    nya_assert(entity != nullptr);
    nya_assert(command != nullptr);

    /*
     * Deterministic given (entity, command, dt), and nothing else.
     */
    f32x2 direction = { 0.0F, 0.0F };

    if (nya_net_command_holds(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_LEFT))) direction.x -= 1.0F;
    if (nya_net_command_holds(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_RIGHT))) direction.x += 1.0F;
    if (nya_net_command_holds(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_UP))) direction.y -= 1.0F;
    if (nya_net_command_holds(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_DOWN))) direction.y += 1.0F;

    if (direction.x == 0.0F && direction.y == 0.0F) return;

    /*
     * Normalised, so diagonal movement is not faster than orthogonal.
     */
    f32 length = sqrtf((direction.x * direction.x) + (direction.y * direction.y));

    direction.x /= length;
    direction.y /= length;

    /*
     * Written straight onto the transform rather than through the solver.
     */
    // from the config file, which server and client read alike, so prediction still agrees. Zero is a field the
    // file left out.
    f32 speed = NYA_CONFIG.game.player_speed > 0.0F ? NYA_CONFIG.game.player_speed : GNY_PLAYER_SPEED;

    entity->position.x += direction.x * speed * delta_time_s;
    entity->position.y += direction.y * speed * delta_time_s;
}

void gny_net_sample_command(OUT NYA_NetCommand* command) {
    nya_assert(command != nullptr);

    /*
     * Read from the merged input view rather than a player slot.
     */
    nya_net_command_set(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_LEFT), nya_input_action_pressed(GNY_ACTION_MOVE_LEFT));
    nya_net_command_set(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_RIGHT), nya_input_action_pressed(GNY_ACTION_MOVE_RIGHT));
    nya_net_command_set(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_UP), nya_input_action_pressed(GNY_ACTION_MOVE_UP));
    nya_net_command_set(command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_DOWN), nya_input_action_pressed(GNY_ACTION_MOVE_DOWN));

    command->aim = nya_input_mouse_position();
}

NYA_EntityHandle gny_net_spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
    nya_unused(name);

    f32 spacing = NYA_CONFIG.game.player_spawn_spacing > 0.0F ? NYA_CONFIG.game.player_spawn_spacing : GNY_PLAYER_SPAWN_SPACING;

    // no physics body: movement is written by the command function, identically on both sides.
    return nya_entity_spawn(
        .name      = "player",
        .type      = GNY_ENTITY_PLAYER,
        .flags     = GNY_FLAG_REPLICATED | GNY_ENTITY_FLAG_AUDIBLE,
        .position  = { (f32)peer.index * spacing, 0.0F, 0.0F },
        .state     = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_render = nya_callback(gny_net_player_on_render)
    );
}

void gny_net_player_on_render(NYA_Entity* entity, NYA_Window* window) {
    nya_assert(entity != nullptr);

    /*
     * Drawn here rather than left to the crate renderer, which is what it used to reuse.
     */
    f32x2 center = nya_entity_render_position(entity).xy;
    f32x2 size   = { GNY_PLAYER_SIZE, GNY_PLAYER_SIZE };

    /*
     * Keyed on the slot like the crates are, so two players are reliably different colours and each
     * keeps its own for as long as it is connected.
     */
    f32 hue = (f32)(((u64)entity->handle.index * 47U) % 360U);

    NYA_Color color = nya_color_from_hsv((NYA_ColorHSV){ .h = hue, .s = 0.75F, .v = 1.0F, .a = 1.0F });

    /*
     * The rotated pair at rotation zero, rather than nya_render2d_rect.
     */
    nya_render2d_rect_rotated(window, center, size, 0.0F, color);
    nya_render2d_rect_rotated_outline(window, center, size, 0.0F, 2.0F, nya_color_darken(color, 0.6F));
}
