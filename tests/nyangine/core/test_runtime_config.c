/**
 * Runtime config: a reflected struct loaded from a .nya file, and kept in sync with it live.
 *
 * Named test_runtime_config rather than test_config: tests/nyangine/net/test_config.c already exists
 * for an unrelated networking config, and the two would otherwise both match a `test_config` filter.
 *
 * NYA_ASSET_HOT_RELOAD is defined here rather than relied upon, for the same reason
 * test_i18n_reload.c defines it: FLAGS_TEST does not set it, so without this the watch and everything
 * it compares would be compiled out and the reload test would pass by testing nothing.
 *
 * Includes gnyame's own translation unit, not just the engine's, because GNY_Config is what the
 * generated reflection table describes for a game struct — see test_reflection_generated.c, which
 * does the same and explains why in its own header.
 **/

// Before the engine, so the watch and the fields it reads are compiled in. See the note above.
#ifndef NYA_ASSET_HOT_RELOAD
#define NYA_ASSET_HOT_RELOAD
#endif

#include "nyangine/nyangine.h"
#include "gnyame/gnyame.h"

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

/** A path of its own under assets/config, so no file this test is not responsible for is touched. */
#define FIXTURE_PATH "./assets/config/__test_runtime_config.nya"

/** A config a hand written .nya file is not required to differ from — see nya_config_load's tolerance
 *  for a file that omits a field, which every fixture below relies on by only ever writing both. */
static void write_fixture(NYA_ConstCString text) {
  NYA_EXPECT(nya_file_write(FIXTURE_PATH, text));
}

/**
 * One frame's worth of the end-of-frame work, which is where the reload pass lives. See
 * test_i18n_reload.c's identical helper for why the clock has to move as well as the event fire:
 * nya_asset_get throttles its stat on frame_stats.uptime_ns, which only a real frame advances.
 * */
static void end_frame(void) {
  nya_app_get()->frame_stats.uptime_ns = nya_clock_get_monotonic_ns();

  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

s32 main(void) {
  // No real audio device; nya_system_asset_init brings up SDL_mixer. Same reason as test_i18n.c.
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();
  nya_system_config_init();

  defer nya_system_config_deinit();
  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the shipped starter file loads into a fresh GNY_Config with its real values
  // ─────────────────────────────────────────────────────────────────────────────
  /*
   * GNY_CONFIG_FILE holds the whole of NYA_CONFIG — its top level is "engine" and "game", matching
   * GNY_Config's own two fields — because gny_world_create loads it in exactly one call, the same one
   * exercised here. NYA_ConfigEngine's fields ("renderer", "physics") are therefore one level down,
   * at config.engine.*, not at the file's top level; the next test loads NYA_ConfigEngine on its own,
   * from a fixture shaped for it instead.
   */
  printf("TEST: nya_config_load reads assets/config/engine.nya\n");
  {
    GNY_Config config = { 0 };
    NYA_EXPECT(nya_config_load(GNY_CONFIG_FILE, nya_reflect_of(GNY_Config), &config));

    nya_assert(config.engine.renderer.shadow_bias == 0.0015F, "shadow_bias, got %f", (double)config.engine.renderer.shadow_bias);
    nya_assert(config.engine.renderer.shadow_cascades == 3, "shadow_cascades, got %u", config.engine.renderer.shadow_cascades);
    nya_assert(config.engine.renderer.shadow_map_size == 1024, "shadow_map_size, got %u", config.engine.renderer.shadow_map_size);
    nya_assert(config.engine.physics.gravity == 9.81F, "gravity, got %f", (double)config.engine.physics.gravity);
    nya_assert(config.engine.physics.sub_steps == 4, "sub_steps, got %u", config.engine.physics.sub_steps);
    nya_assert(config.game.player_speed == 220.0F, "player_speed, got %f", (double)config.game.player_speed);
    nya_assert(config.game.player_spawn_spacing == 64.0F, "player_spawn_spacing, got %f",
               (double)config.game.player_spawn_spacing);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_ConfigEngine loads on its own, from a file shaped for it rather than for GNY_Config
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_config_load resolves a nested struct on its own\n");
  {
    write_fixture("nya 2 0\n"
                  "{\n"
                  "    renderer: object {\n"
                  "        shadow_bias: f32 0.002;\n"
                  "        shadow_cascades: u32 2;\n"
                  "        shadow_map_size: u32 2048;\n"
                  "    };\n"
                  "    physics: object {\n"
                  "        gravity: f32 12.5;\n"
                  "        sub_steps: u32 6;\n"
                  "    };\n"
                  "}\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_ConfigEngine engine = { 0 };
    NYA_EXPECT(nya_config_load(FIXTURE_PATH, nya_reflect_of(NYA_ConfigEngine), &engine));

    nya_assert(engine.renderer.shadow_cascades == 2, "shadow_cascades, got %u", engine.renderer.shadow_cascades);
    nya_assert(engine.renderer.shadow_map_size == 2048, "shadow_map_size, got %u", engine.renderer.shadow_map_size);
    nya_assert(engine.physics.sub_steps == 6, "sub_steps, got %u", engine.physics.sub_steps);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a missing file fails cleanly, without touching the instance
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a missing file returns an error and changes nothing\n");
  {
    NYA_ConfigEngine sentinel = {
      .renderer = { .shadow_bias = 7.0F, .shadow_cascades = 7, .shadow_map_size = 7 },
      .physics  = { .gravity = 7.0F, .sub_steps = 7 },
    };
    NYA_ConfigEngine instance = sentinel;

    NYA_Error result = nya_config_load("./assets/config/__does_not_exist.nya", nya_reflect_of(NYA_ConfigEngine), &instance);

    nya_assert(!result.ok, "a missing file should not report success");
    nya_assert(nya_memcmp(&instance, &sentinel, sizeof(sentinel)) == 0, "a failed load must not touch the instance");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a malformed file fails cleanly, without touching the instance
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a malformed file returns an error and changes nothing\n");
  {
    // Unterminated: no closing braces. What an editor's save looks like caught mid write.
    write_fixture("nya 2 0\n{\n    renderer: object {\n        shadow_bias: f32 0.5;\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_ConfigEngine sentinel = {
      .renderer = { .shadow_bias = 5.0F, .shadow_cascades = 5, .shadow_map_size = 5 },
      .physics  = { .gravity = 5.0F, .sub_steps = 5 },
    };
    NYA_ConfigEngine instance = sentinel;

    NYA_Error result = nya_config_load(FIXTURE_PATH, nya_reflect_of(NYA_ConfigEngine), &instance);

    nya_assert(!result.ok, "malformed content should fail to parse");
    nya_assert(nya_memcmp(&instance, &sentinel, sizeof(sentinel)) == 0, "a failed parse must not touch the instance");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_config_watch loads once, then an edit on disk is picked up live
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_config_watch reloads on an edit\n");
  {
    write_fixture("nya 2 0\n"
                  "{\n"
                  "    renderer: object {\n"
                  "        shadow_bias: f32 0.0015;\n"
                  "        shadow_cascades: u32 3;\n"
                  "        shadow_map_size: u32 1024;\n"
                  "    };\n"
                  "    physics: object {\n"
                  "        gravity: f32 9.81;\n"
                  "        sub_steps: u32 4;\n"
                  "    };\n"
                  "}\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_ConfigEngine engine = { 0 };
    NYA_EXPECT(nya_config_watch(FIXTURE_PATH, nya_reflect_of(NYA_ConfigEngine), &engine));

    nya_assert(engine.physics.sub_steps == 4, "the initial load, got %u", engine.physics.sub_steps);

    // The edit a developer would make, changing one field and leaving the rest.
    write_fixture("nya 2 0\n"
                  "{\n"
                  "    renderer: object {\n"
                  "        shadow_bias: f32 0.0015;\n"
                  "        shadow_cascades: u32 3;\n"
                  "        shadow_map_size: u32 1024;\n"
                  "    };\n"
                  "    physics: object {\n"
                  "        gravity: f32 9.81;\n"
                  "        sub_steps: u32 8;\n"
                  "    };\n"
                  "}\n");

    /*
     * Driven rather than waited on, and it takes more than one frame by design — see the identical
     * note in test_i18n_reload.c: nya_asset_get stats at most once per stat interval, and the reload
     * pass waits for the timestamp to settle before trusting it.
     */
    b8 reloaded = false;

    for (u32 frame = 0; frame < 40 && !reloaded; frame++) {
      SDL_Delay(20);
      end_frame();

      reloaded = engine.physics.sub_steps == 8;
    }

    nya_assert(reloaded, "the edit should have been picked up, got %u", engine.physics.sub_steps);

    // A field the edit did not touch has to still be there: the reload re-resolves the whole file
    // rather than patching one key.
    nya_assert(engine.renderer.shadow_map_size == 1024, "the untouched field survived, got %u", engine.renderer.shadow_map_size);

    printf("  PASSED\n");
  }

  printf("PASSED: test_runtime_config\n");

  return 0;
}
