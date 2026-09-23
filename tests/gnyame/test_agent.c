/**
 * DQN and NEAT playing gnyame as a person does: keys and the mouse through the event queue, into the
 * real menus, with the assertions as the oracle and the seed as the bug report.
 *
 * Run with no arguments it plays a short seeded run with each kind, which is what `./build run test`
 * does: short enough to be a test, long enough that the three policies really drive the application.
 * Run with `--kind` and `--episodes` it trains, which is what `./build run agent` does.
 *
 * The scene layers are stubs, as they are in test_screens, because building one needs a GPU device
 * this run does not have. What the agent drives is everything above them: the title screen, the pause
 * menu and its rows, the screen stack and every key and click that reaches them. A crash in a menu is
 * a crash, and that is where this looks.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** Ticks one episode plays in the regression run. Twenty simulated seconds of pressing things. */
#define REGRESSION_TICKS 1200

/** Episodes the regression run plays per kind. Two, so a DQN gets a round of learning between them. */
#define REGRESSION_EPISODES 2

/** Genomes per generation in the regression run. Small: a generation is that many sessions. */
#define REGRESSION_POPULATION 4

/**
 * Wall clock the regression run may take. It takes about 2 s sanitized on the 8 thread dev machine, and
 * hung twice on 2026-09-22 (one instance for 5h32m), so 60 s is thirty times the normal run and still
 * fails a hang well inside the CI job's own timeout.
 * */
#define REGRESSION_DEADLINE_S 60

/** A seed nobody chose, for a run nobody is replaying. Printed by the run itself. */
static u64 seed_fresh(void) {
    u64 now = nya_clock_get_monotonic_ns();
    return nya_hash_wyhash(&now, sizeof(now));
}

/**
 * The one thing a scene layer does that a stub has to keep: cancel goes back.
 *
 * Found by the agent rather than reasoned about. With the scenes stubbed down to an id, the 3D scene
 * was a screen nothing could leave: the real gny_layer_cube3d_on_event maps cancel to the main menu
 * and the stub did not, so every agent that wandered in spent the rest of its run there and scored
 * nothing. The rest of that hook regenerates terrain and clears cubes, which needs the GPU state this
 * run has none of, so this stands in for the navigation and nothing else.
 * */
static void scene_stub_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    if (event->type != NYA_EVENT_KEY_DOWN || event->as_key_event.is_repeat) return;
    if (!nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, event->as_key_event.key, event->as_key_event.modifier_flags)) return;

    gny_screen_request(GNY_SCREEN_MAIN_MENU);
    event->was_handled = true;
}

/** A layer with an id and no hooks, standing in for the scenes, which build physics and GPU state. */
static NYA_Layer layer_stub(NYA_ConstCString id) {
    NYA_Layer layer = { .enabled = true };
    (void)snprintf(layer.id, sizeof(layer.id), "%s", id);
    return layer;
}

s32 main(s32 argc, NYA_CString argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_AgentKind kind     = NYA_AGENT_KIND_COUNT;
    u64           seed     = 0;
    u32           episodes = 0;
    u64           ticks    = 0;
    b8            verbose  = false;

    for (s32 i = 1; i < argc; i++) {
        if (nya_string_equals(argv[i], "--verbose")) {
            verbose = true;
        } else if (nya_string_equals(argv[i], "--kind") && i + 1 < argc) {
            kind = nya_agent_kind_from_name(argv[++i]);

            if (kind == NYA_AGENT_KIND_COUNT) {
                (void)fprintf(stderr, "Error: '%s' is not an agent kind. Use random, dqn or neat.\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else if (nya_string_equals(argv[i], "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], nullptr, 0);
        } else if (nya_string_equals(argv[i], "--episodes") && i + 1 < argc) {
            episodes = (u32)strtoul(argv[++i], nullptr, 0);
        } else if (nya_string_equals(argv[i], "--ticks") && i + 1 < argc) {
            ticks = strtoull(argv[++i], nullptr, 0);
        } else {
            (void)fprintf(stderr, "Error: unexpected argument '%s'\n\n", argv[i]);
            (void)fprintf(stderr, "Usage: test_agent [--kind random|dqn|neat] [--seed <n>] [--episodes <n>] [--ticks <n>] [--verbose]\n");
            return EXIT_FAILURE;
        }
    }

    if (seed == 0) seed = seed_fresh();

    // armed before anything comes up and stopped after everything goes down, since a hang in bring-up
    // or teardown is still a hang. A training run is as long as whoever started it asked for.
    if (kind == NYA_AGENT_KIND_COUNT) nya_test_deadline_start("test_agent", REGRESSION_DEADLINE_S);
    defer nya_test_deadline_stop();

    // no display: the agent plays headless, which is the whole point of playing it
    // a thousand times faster than a person could.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // settings and saves load from the data directory, so a scratch one keeps the player's own bindings
    // and volumes out of a run that presses every key it can find.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_agent_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-agent"));
    (void)nya_filesystem_delete_recursive(data_home);

    nya_assert(nya_host_environment_add("XDG_DATA_HOME", data_home));
    nya_assert(nya_host_environment_add("APPDATA", data_home));

    defer nya_arena_destroy(scratch);
    defer (void)nya_filesystem_delete_recursive(data_home);

    /*
     * The real application, headless: every engine subsystem the game has, brought up in the order the
     * game brings them up in, with the renderer reported unavailable rather than fatal.
     */
    NYA_EXPECT(nya_app_init(.headless = true, .app_id = "gnyame"));
    defer nya_app_deinit();

    gny_actions_init();
    defer gny_actions_deinit();

    NYA_World* engine_world = nya_world_create();
    (void)nya_world_set(engine_world);
    defer nya_world_destroy(engine_world);

    // the parts of gny_world_create the menus reach, without the Lua VM, the robots and the scenes.
    GNY_World* world = nya_arena_alloc(engine_world->allocator, sizeof(GNY_World));
    *world           = (GNY_World){ .window_main = NYA_WINDOW_HANDLE_NONE, .terrain = NYA_ENTITY_HANDLE_NONE, .terrain_seed = 1 };
    nya_world_user_data_set(world);

    gny_fonts_register();
    gny_layers_init();
    NYA_EXPECT(nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT));

    // what assets/config/engine.nya sets for the menus, without watching the file.
    nya_config_engine()->ui = (NYA_UIStyle){ .font       = "menu",
                                             .title_font = "menu_title",
                                             .body_size  = GNY_MENU_ITEM_SIZE,
                                             .title_size = GNY_MENU_TITLE_SIZE,
                                             .item_height = 42.0F };

    GNY_LAYER_GAME   = layer_stub(GNY_LAYER_GAME_ID);
    GNY_LAYER_UI     = layer_stub(GNY_LAYER_UI_ID);
    GNY_LAYER_CUBE3D = layer_stub(GNY_LAYER_CUBE3D_ID);

    // the HUD's update is the pause key and nothing else, so it is the one hook a stub keeps: without
    // it the agent walks into the 2D scene and can never leave, which is not what the application does.
    GNY_LAYER_UI.on_update = nya_callback(gny_layer_ui_on_update);

    // and the same for the 3D scene, which has no HUD layer of its own. See scene_stub_on_event.
    GNY_LAYER_CUBE3D.on_event = nya_callback(scene_stub_on_event);

    world->window_main = nya_window_create("agent", WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(world->window_main));

    NYA_Window* window    = nya_window_get(GNY_WINDOW_MAIN);
    window->screen_width  = WINDOW_WIDTH;
    window->screen_height = WINDOW_HEIGHT;

    // the title screen, which is where a player starts and therefore where the agent does.
    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_BACKGROUND);
    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);

    NYA_EXPECT(nya_system_registry_finalize());

    u32 failures = 0;

    if (kind != NYA_AGENT_KIND_COUNT) {
        failures = gny_agent_run((GNY_AgentRun){ .kind       = kind,
                                                 .seed       = seed,
                                                 .episodes   = episodes,
                                                 .tick_count = ticks,
                                                 .verbose    = verbose });
    } else {
        /*
         * No kind named: the regression run, which is what `./build run test` executes. Every kind
         * plays, because what is being tested is that each of them can drive the application at all,
         * not that any of them has learned anything in two episodes.
         */
        printf("TEST: three agent kinds, %d episodes of %d ticks each, seed 0x%016llX\n", REGRESSION_EPISODES, REGRESSION_TICKS,
               (unsigned long long)seed);

        for (u32 i = 0; i < (u32)NYA_AGENT_KIND_COUNT; i++) {
            failures += gny_agent_run((GNY_AgentRun){ .kind       = (NYA_AgentKind)i,
                                                      .seed       = seed,
                                                      .episodes   = REGRESSION_EPISODES,
                                                      .tick_count = REGRESSION_TICKS,
                                                      .population = REGRESSION_POPULATION,
                                                      .verbose    = verbose });
        }
    }

    if (failures > 0) {
        printf("FAILED: test_agent (%u failures)\n", failures);
        return EXIT_FAILURE;
    }

    printf("PASSED: test_agent (0 failures)\n");

    return EXIT_SUCCESS;
}
