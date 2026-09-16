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
    if (GNY_LAUNCH.role == NYA_NET_ROLE_CLIENT) {
        NYA_Error connected = nya_net_client_connect(GNY_LAUNCH.address, GNY_LAUNCH.port, GNY_LAUNCH.name, (NYA_NetClientConfig){
            .replicated_flag   = GNY_FLAG_REPLICATED,
            .on_apply_command  = nya_callback(gny_net_apply_command),
            .on_sample_command = nya_callback(gny_net_sample_command),
        });

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

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
        .replicated_flag  = GNY_FLAG_REPLICATED,
        .max_players      = GNY_LAUNCH.max_players,
        .on_spawn_player  = nya_callback(gny_net_spawn_player),
        .on_apply_command = nya_callback(gny_net_apply_command),
    }), "while starting the server");

    // Single player is this same server with nothing after it. That is the whole architecture; see net.h.
    if (GNY_LAUNCH.listen_port != 0) {
        NYA_Error listening = nya_net_server_listen(GNY_LAUNCH.listen_port);

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
    entity->position.x += direction.x * GNY_PLAYER_SPEED * delta_time_s;
    entity->position.y += direction.y * GNY_PLAYER_SPEED * delta_time_s;
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

    /*
     * No physics body, deliberately.
     */
    return nya_entity_spawn(
        .name = "player",
        /*
         * GNY_ENTITY_PLAYER, not GNY_ENTITY_BOX.
         */
        .type      = GNY_ENTITY_PLAYER,
        .flags     = GNY_FLAG_REPLICATED | GNY_ENTITY_FLAG_AUDIBLE,
        .position  = { (f32)peer.index * GNY_PLAYER_SPAWN_SPACING, 0.0F, 0.0F },
        .state     = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_render = nya_callback(gny_net_player_on_render)
    );
}

void gny_net_player_on_render(NYA_Entity* entity, NYA_Window* window) {
    nya_assert(entity != nullptr);

    /*
     * Drawn here rather than left to the crate renderer, which is what it used to reuse.
     */
    f32x2 center = { entity->position.x, entity->position.y };
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
