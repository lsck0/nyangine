/**
 * @file examples/pong_multiplayer/main.c
 *
 * Two dimensional pong over the engine's netcode: one server that owns the ball, clients that
 * predict their own paddle and interpolate everyone else's.
 *
 * ```
 * ./build run example pong_multiplayer                      # one player, listening on nobody
 * ./pong_multiplayer.example --listen 27015                 # host a game
 * ./pong_multiplayer.example --connect 127.0.0.1 27015      # join it from another terminal
 * ```
 *
 * `w`/`s` or the arrow keys move your paddle.
 *
 * ## The one architecture
 *
 * There is no "single player mode". A lone player is a server with nobody connected to it, and the
 * local player reaches it through a loopback transport — the same client code a remote player runs.
 * Opening the game to the network adds `nya_net_server_listen` and changes nothing else. See
 * `src/nyangine/net/net.h`.
 *
 * Prediction works because `pong_apply_command` is the only thing that moves a paddle, and both
 * ends call it with the same command and the same fixed timestep. Put movement anywhere else and
 * the client's guess stops matching the server's answer.
 *
 * The ball is not predicted: only the server integrates it, and clients draw the replica the
 * snapshots carry.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define WINDOW_TITLE  "nyangine — pong"
#define WINDOW_WIDTH  960
#define WINDOW_HEIGHT 540

/**
 * The court, in world units, with the origin at its centre. Everything below is in these units and
 * the renderer is handed a camera that maps them onto the window.
 * */
#define COURT_HALF_WIDTH  480.0F
#define COURT_HALF_HEIGHT 270.0F

#define PADDLE_WIDTH  16.0F
#define PADDLE_HEIGHT 96.0F

/** World units per second. A paddle crosses half the court in about one second. */
#define PADDLE_SPEED 320.0F

/** How far in from each wall a paddle sits. Two paddles, two walls, one number. */
#define PADDLE_INSET 48.0F

#define BALL_RADIUS 10.0F

/** Starting speed. It does not accelerate: this is an example, not a game. */
#define BALL_SPEED 260.0F

/** What the server tells the client the fastest a paddle may move is, with headroom for rounding. */
#define PADDLE_SPEED_LIMIT (PADDLE_SPEED * 1.25F)

/** The tick. Sixty-two a second, matching the engine's default 16 ms step. */
#define TICK_MS 16

/** Text size for the score. */
#define SCORE_POINT_SIZE 32.0F

/** Which bit of NYA_Entity.flags marks an entity the server replicates. Any free bit will do. */
#define FLAG_REPLICATED (1ULL << 20)

/** The layer's id. Compared by content, so it survives a code reload. */
#define LAYER_ID "pong"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a thing is. `type` on NYA_Entity, and what the draw switches on. */
typedef enum {
    PONG_ENTITY_NONE = 0,
    PONG_ENTITY_PADDLE,
    PONG_ENTITY_BALL,

    PONG_ENTITY_KIND_COUNT,
} PongEntityKind;

/**
 * The player's actions, continuing the engine's numbering from NYA_INPUT_ACTION_USER.
 *
 * Unnamed, so the members are plain integers usable wherever an NYA_InputAction is wanted. A named
 * enum here would be a second enumeration type and every call would need a cast
 * (-Wimplicit-enum-enum-cast is an error in this tree).
 * */
enum {
    PONG_ACTION_UP = NYA_INPUT_ACTION_USER,
    PONG_ACTION_DOWN,
};

/** Where an action sits in NYA_NetCommand.actions. */
#define COMMAND_BIT(action) ((u32)((action) - NYA_INPUT_ACTION_USER))

/** The game's state, hung off the engine world so it shares the world's lifetime. */
typedef struct {
    NYA_WindowHandle window;
    NYA_EntityHandle ball;

    /** Left player's, then right player's. Indexed by the paddle's side. */
    u32 scores[2];

    NYA_NetLaunchConfig launch;
} Pong;

NYA_INTERNAL Pong* pong(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MOVEMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The court edge a paddle's centre may not pass. */
#define PADDLE_LIMIT_Y (COURT_HALF_HEIGHT - (PADDLE_HEIGHT * 0.5F))

/**
 * The only thing that moves a paddle, on the server and on every client alike.
 *
 * Deterministic given (entity, command, delta_time_s) and nothing else: no clock, no random, no
 * reading of another entity. The moment it depends on something the client does not have, the
 * client's prediction diverges and the server starts correcting it every tick.
 * */
void pong_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    nya_assert(entity != nullptr);
    nya_assert(command != nullptr);

    f32 direction = 0.0F;

    if (nya_net_command_holds(command, COMMAND_BIT(PONG_ACTION_UP))) direction -= 1.0F;
    if (nya_net_command_holds(command, COMMAND_BIT(PONG_ACTION_DOWN))) direction += 1.0F;

    if (direction == 0.0F) return;

    entity->position.y = nya_clamp(entity->position.y + (direction * PADDLE_SPEED * delta_time_s), -PADDLE_LIMIT_Y, PADDLE_LIMIT_Y);
}

/** What the local player is doing this tick, read once and sent to the server. */
void pong_sample_command(OUT NYA_NetCommand* command) {
    nya_assert(command != nullptr);

    nya_net_command_set(command, COMMAND_BIT(PONG_ACTION_UP), nya_input_action_pressed(PONG_ACTION_UP));
    nya_net_command_set(command, COMMAND_BIT(PONG_ACTION_DOWN), nya_input_action_pressed(PONG_ACTION_DOWN));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENTITIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Gives a joining player a paddle. The server's, called once per peer.
 *
 * Peers alternate sides, so the first player is on the left and the second faces them. A third
 * would stack on the left again, which is fine for an example and would not be for a game.
 * */
NYA_EntityHandle pong_spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
    nya_unused(name);

    f32 x = (peer.index % 2 == 0) ? -(COURT_HALF_WIDTH - PADDLE_INSET) : (COURT_HALF_WIDTH - PADDLE_INSET);

    return nya_entity_spawn(
        .name     = "paddle",
        .type     = PONG_ENTITY_PADDLE,
        .flags    = FLAG_REPLICATED,
        .position = { x, 0.0F, 0.0F },
    );
}

/**
 * Puts the ball back in the middle, heading toward `toward_x`, which is -1 for the left end.
 *
 * Served back toward the end it did not just leave through, so the player who conceded gets it. It
 * also keeps a lone player in a rally: the first peer's paddle is on the left, and serving right
 * into an empty half would just score again on the next tick.
 * */
NYA_INTERNAL void ball_serve(NYA_Entity* ball, f32 toward_x) {
    nya_assert(ball != nullptr);

    ball->position = (f32x3){ 0.0F, 0.0F, 0.0F };

    // A shallow diagonal rather than straight across, so the first bounce happens quickly and the
    // walls are visibly doing something.
    ball->velocity = (f32x3){ toward_x * BALL_SPEED * 0.8F, BALL_SPEED * 0.6F, 0.0F };

    // Teleported, not moved: without this the renderer interpolates from the old side of the court
    // to the new one and the ball streaks across the screen on the serve frame.
    nya_entity_transform_snap(ball);
}

/**
 * The ball, integrated by the server alone. Clients see it through snapshots.
 * */
NYA_INTERNAL void ball_update(f32 delta_time_s) {
    NYA_Entity* ball = nya_entity_get(pong()->ball);
    if (ball == nullptr) return;

    ball->position.x += ball->velocity.x * delta_time_s;
    ball->position.y += ball->velocity.y * delta_time_s;

    // ── the top and bottom walls ────────────────────────────────────────────────────────────────
    f32 limit_y = COURT_HALF_HEIGHT - BALL_RADIUS;

    if (ball->position.y < -limit_y || ball->position.y > limit_y) {
        ball->position.y = nya_clamp(ball->position.y, -limit_y, limit_y);
        ball->velocity.y = -ball->velocity.y;
    }

    // ── the paddles ─────────────────────────────────────────────────────────────────────────────
    nya_entity_foreach_kind (PONG_ENTITY_PADDLE, paddle) {
        b8 overlaps_x = fabsf(ball->position.x - paddle->position.x) < ((PADDLE_WIDTH * 0.5F) + BALL_RADIUS);
        b8 overlaps_y = fabsf(ball->position.y - paddle->position.y) < ((PADDLE_HEIGHT * 0.5F) + BALL_RADIUS);

        if (!overlaps_x || !overlaps_y) continue;

        // Only when the ball is closing on the paddle. Without this a ball that clipped inside
        // flips its velocity every tick and sticks to the face.
        b8 closing = (paddle->position.x < 0.0F) ? ball->velocity.x < 0.0F : ball->velocity.x > 0.0F;
        if (!closing) continue;

        ball->velocity.x = -ball->velocity.x;

        // Where it hit the paddle steers it, which is the whole depth of pong.
        f32 offset       = (ball->position.y - paddle->position.y) / (PADDLE_HEIGHT * 0.5F);
        ball->velocity.y = nya_clamp(offset, -1.0F, 1.0F) * BALL_SPEED;
    }

    // ── the left and right walls ────────────────────────────────────────────────────────────────
    if (ball->position.x < -COURT_HALF_WIDTH) {
        pong()->scores[1]++;
        ball_serve(ball, 1.0F);
    } else if (ball->position.x > COURT_HALF_WIDTH) {
        pong()->scores[0]++;
        ball_serve(ball, -1.0F);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE LAYER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void pong_layer_on_create(NYA_Window* window) {
    nya_unused(window);

    nya_input_action_bind(PONG_ACTION_UP, NYA_KEY_W);
    nya_input_action_bind(PONG_ACTION_UP, NYA_KEY_UP);
    nya_input_action_bind(PONG_ACTION_DOWN, NYA_KEY_S);
    nya_input_action_bind(PONG_ACTION_DOWN, NYA_KEY_DOWN);

    // The server owns the ball, so a pure client never spawns one; it arrives in a snapshot.
    if (!nya_net_server_running()) return;

    pong()->ball = nya_entity_spawn(
        .name     = "ball",
        .type     = PONG_ENTITY_BALL,
        .flags    = FLAG_REPLICATED,
        .position = { 0.0F, 0.0F, 0.0F },
    );

    NYA_Entity* ball = nya_entity_get(pong()->ball);
    nya_assert(ball != nullptr, "the entity table was full on the first spawn");

    // toward the left, where the first peer's paddle is.
    ball_serve(ball, -1.0F);
}

void pong_layer_on_destroy(NYA_Window* window) {
    nya_unused(window);

    // The world owns the entities and tears them down with itself; there is nothing of this
    // layer's own to release. The pair exists so the day there is, callers already pair it.
}

void pong_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    if (event->type != NYA_EVENT_KEY_DOWN) return;

    if (event->as_key_event.key == NYA_KEY_ESCAPE) {
        nya_app_get()->should_quit = true;
        event->was_handled         = true;
    }
}

void pong_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    // Only where the authority is. On a client this is false and the ball is whatever the last
    // snapshot said, interpolated.
    if (nya_net_server_running()) ball_update(delta_time_s);
}

/** Court lines, paddles, ball, score. Screen space; the court is centred on the window. */
void pong_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    // Smoothing between ticks, before anything is read: replicas are placed for this frame's time,
    // not for the last tick's.
    nya_net_client_interpolate((f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));

    f32 center_x = (f32)window->width * 0.5F;
    f32 center_y = (f32)window->height * 0.5F;

    nya_render2d_rect(window, 0.0F, 0.0F, (f32)window->width, (f32)window->height, NYA_COLOR_BLACK);

    // The halfway line, as a dashed run of short bars.
    for (f32 y = center_y - COURT_HALF_HEIGHT; y < center_y + COURT_HALF_HEIGHT; y += 24.0F) {
        nya_render2d_rect(window, center_x - 1.0F, y, 2.0F, 12.0F, NYA_COLOR_DARK_GRAY);
    }

    nya_render2d_rect_outline(window, center_x - COURT_HALF_WIDTH, center_y - COURT_HALF_HEIGHT, COURT_HALF_WIDTH * 2.0F, COURT_HALF_HEIGHT * 2.0F,
                              2.0F, NYA_COLOR_DARK_GRAY);

    // nya_entity_render_position, not entity->position: the former is where the entity is *now*,
    // between the last tick and the next, which is what a frame should draw.
    nya_entity_foreach_kind (PONG_ENTITY_PADDLE, paddle) {
        f32x3 at = nya_entity_render_position(paddle);

        // The player's own paddle is brighter, so it is obvious which one prediction is moving.
        b8 is_mine = nya_entity_get(nya_net_client_entity()) == paddle;

        nya_render2d_rect(window, center_x + at.x - (PADDLE_WIDTH * 0.5F), center_y + at.y - (PADDLE_HEIGHT * 0.5F), PADDLE_WIDTH, PADDLE_HEIGHT,
                          is_mine ? NYA_COLOR_WHITE : NYA_COLOR_GRAY);
    }

    nya_entity_foreach_kind (PONG_ENTITY_BALL, ball) {
        f32x3 at = nya_entity_render_position(ball);

        nya_render2d_circle(window, (f32x2){ center_x + at.x, center_y + at.y }, BALL_RADIUS, NYA_COLOR_WHITE);
    }

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, SCORE_POINT_SIZE, center_x - 120.0F, center_y - COURT_HALF_HEIGHT + 16.0F,
                                 NYA_COLOR_LIGHT_GRAY, "%u", pong()->scores[0]);

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, SCORE_POINT_SIZE, center_x + 100.0F, center_y - COURT_HALF_HEIGHT + 16.0F,
                                 NYA_COLOR_LIGHT_GRAY, "%u", pong()->scores[1]);

    // What the link is costing, which is the reason to run this example twice at once.
    NYA_NetPeerStats stats = nya_net_client_stats();

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, 14.0F, 12.0F, 12.0F, NYA_COLOR_GRAY, "rtt %.0f ms · loss %.0f%% · %llu corrections",
                                 (f64)stats.rtt_ms, (f64)stats.packet_loss * 100.0, (unsigned long long)nya_net_client_correction_count());
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WIRING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Connects as a client, or becomes the server. The four modes of net.h, in one function. */
NYA_INTERNAL void pong_net_start(void) {
    NYA_NetLaunchConfig* launch = &pong()->launch;

    if (launch->role == NYA_NET_ROLE_CLIENT) {
        NYA_Error connected = nya_net_client_connect(launch->address, launch->port, launch->name, (NYA_NetClientConfig){
            .replicated_flag   = FLAG_REPLICATED,
            .on_apply_command  = nya_callback(pong_apply_command),
            .on_sample_command = nya_callback(pong_sample_command),
            .conditions        = launch->conditions,
        });

        if (connected.ok) return;

        // A server that is not there is an operating error, not a crash. Fall back to hosting.
        nya_log_error("Could not reach %s:%u (%s); hosting instead.", launch->address, launch->port, (NYA_ConstCString)connected.message);

        launch->role = NYA_NET_ROLE_SERVER;
    }

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
        .replicated_flag  = FLAG_REPLICATED,
        .on_spawn_player  = nya_callback(pong_spawn_player),
        .on_apply_command = nya_callback(pong_apply_command),
        .max_speed        = PADDLE_SPEED_LIMIT,
        .conditions       = launch->conditions,
    }), "while starting the server");

    // Listening is the only difference between a lone player and a host. The world does not change.
    if (launch->listen_port != 0) {
        NYA_Error listening = nya_net_server_listen(launch->listen_port);

        if (!listening.ok) {
            nya_log_error("Could not open port %u (%s); playing alone.", launch->listen_port, (NYA_ConstCString)listening.message);
        }
    }

    // The local player joins its own server through a loopback transport, running exactly the
    // client code a remote player runs.
    NYA_NetTransport* local = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&local), "while attaching the local player");

    NYA_EXPECT(nya_net_client_attach(local, launch->name, (NYA_NetClientConfig){
        .replicated_flag   = FLAG_REPLICATED,
        .on_apply_command  = nya_callback(pong_apply_command),
        .on_sample_command = nya_callback(pong_sample_command),
    }), "while attaching the local client");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_backtrace_init();

    // --connect, --listen, --port, --name, --net-latency and the rest, parsed by the engine so
    // every nyangine game takes the same flags. It never fails and never exits.
    NYA_NetLaunchConfig launch = nya_net_config_from_args(argc, argv);

    NYA_EXPECT(nya_app_init(.time_step_ns = nya_time_ms_to_ns(TICK_MS), .app_id = "pong"), "while starting the engine");

    // The game's root pointer lives on the world, so it shares the world's arena and lifetime.
    Pong* state = nya_arena_alloc(nya_world()->allocator, sizeof(Pong));
    *state      = (Pong){ .window = NYA_WINDOW_HANDLE_NONE, .ball = NYA_ENTITY_HANDLE_NONE, .launch = launch };
    nya_world_user_data_set(state);

    // Before the window: the layer's on_create spawns the ball, and only a server may.
    pong_net_start();

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(pong_layer, LAYER_ID));

    nya_app_run();

    nya_net_client_disconnect();
    nya_net_server_stop();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
