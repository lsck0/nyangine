/**
 * The game's config fallbacks: a field the file leaves out loads as zero, and zero means the constants.h default.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

/** Its own file under assets/config, removed at the end. */
#define FIXTURE_PATH "./assets/config/__test_gnyame_config.nya"

/** Only the spawn spacing, so player_speed is a field the file left out. */
#define FIXTURE                                                                                                                                    \
    "nya 2 0\n"                                                                                                                                    \
    "{\n"                                                                                                                                          \
    "    game: object {\n"                                                                                                                         \
    "        player_spawn_spacing: f32 10.0;\n"                                                                                                    \
    "    };\n"                                                                                                                                         \
    "}\n"

/** Where one tick of `bits` moves a player standing at the origin. */
static f32x2 moved(u32 bits, f32 delta_time_s) {
    NYA_Entity     entity  = { 0 };
    NYA_NetCommand command = { 0 };

    if (bits & 1) nya_net_command_set(&command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_RIGHT), true);
    if (bits & 2) nya_net_command_set(&command, GNY_COMMAND_BIT(GNY_ACTION_MOVE_DOWN), true);

    gny_net_apply_command(&entity, &command, delta_time_s);

    return (f32x2){ entity.position.x, entity.position.y };
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();
    nya_system_config_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_config_deinit();
    defer nya_world_destroy(world);

    // A field the file leaves out loads as zero, next to one it sets.
    {
        NYA_EXPECT(nya_file_write(FIXTURE_PATH, FIXTURE));

        NYA_CONFIG = (GNY_Config){ 0 };
        NYA_Error loaded = nya_config_load(FIXTURE_PATH, nya_reflect_of(GNY_Config), &NYA_CONFIG);

        (void)nya_filesystem_delete(FIXTURE_PATH);

        nya_check(loaded.ok, "the fixture should load: %s", (NYA_ConstCString)loaded.message);
        nya_check(NYA_CONFIG.game.player_speed == 0.0F, "the omitted speed stays zero, got %f", (f64)NYA_CONFIG.game.player_speed);
        nya_check(NYA_CONFIG.game.player_spawn_spacing == 10.0F, "the spacing is read, got %f", (f64)NYA_CONFIG.game.player_spawn_spacing);
    }

    // Zero speed falls back to GNY_PLAYER_SPEED; a set speed is used; a negative one falls back too.
    {
        NYA_CONFIG.game.player_speed = 0.0F;
        f32x2 fallback               = moved(1, 0.5F);
        nya_check(fabsf(fallback.x - (GNY_PLAYER_SPEED * 0.5F)) < 1e-3F && fallback.y == 0.0F, "zero moves at GNY_PLAYER_SPEED, got %f",
                  (f64)fallback.x);

        NYA_CONFIG.game.player_speed = 100.0F;
        f32x2 configured             = moved(1, 0.5F);
        nya_check(fabsf(configured.x - 50.0F) < 1e-3F, "a configured speed is used, got %f", (f64)configured.x);

        NYA_CONFIG.game.player_speed = -5.0F;
        f32x2 negative               = moved(1, 0.5F);
        nya_check(fabsf(negative.x - (GNY_PLAYER_SPEED * 0.5F)) < 1e-3F, "a negative speed falls back as well, got %f", (f64)negative.x);

        NYA_CONFIG.game.player_speed = 100.0F;
        f32x2 diagonal               = moved(1 | 2, 1.0F);
        nya_check(fabsf(sqrtf((diagonal.x * diagonal.x) + (diagonal.y * diagonal.y)) - 100.0F) < 1e-3F, "diagonals move at the same speed, got %f, %f",
                  (f64)diagonal.x, (f64)diagonal.y);

        nya_check(moved(0, 1.0F).x == 0.0F, "and no input does not move");
    }

    // Zero spawn spacing falls back to GNY_PLAYER_SPAWN_SPACING.
    {
        NYA_CONFIG.game.player_spawn_spacing = 0.0F;
        NYA_Entity* fallback                 = nya_entity_get(gny_net_spawn_player((NYA_NetPeerId){ .index = 3, .generation = 1 }, "fallback"));
        nya_check(fallback != nullptr && fallback->position.x == 3.0F * GNY_PLAYER_SPAWN_SPACING, "slot 3 spawns three default spacings out");

        NYA_CONFIG.game.player_spawn_spacing = 10.0F;
        NYA_Entity* configured               = nya_entity_get(gny_net_spawn_player((NYA_NetPeerId){ .index = 3, .generation = 1 }, "configured"));
        nya_check(configured != nullptr && configured->position.x == 30.0F, "and three configured spacings once one is set");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
