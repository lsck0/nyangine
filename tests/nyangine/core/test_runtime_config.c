/**
 * Runtime config: a reflected struct loaded from a .nya file, and kept in sync with it live.
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

/**
 * A config a hand written .nya file need not match exactly; nya_config_load tolerates omitted fields,
 * but every fixture below writes both.
 * */
static void write_fixture(NYA_ConstCString text) {
  u64 before = 0;
  b8  existed = nya_filesystem_last_modified(FIXTURE_PATH, &before).ok;

  NYA_EXPECT(nya_file_write(FIXTURE_PATH, text));

  // two writes a few milliseconds apart can get the same timestamp from a coarse filesystem clock, and a
  // watch comparing timestamps then never sees the edit. Rewritten until the timestamp moves.
  for (u32 attempt = 0; existed && attempt < 200; attempt++) {
    u64 after = 0;
    NYA_EXPECT(nya_filesystem_last_modified(FIXTURE_PATH, &after));
    if (after != before) break;

    SDL_Delay(5);
    NYA_EXPECT(nya_file_write(FIXTURE_PATH, text));
  }
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
   * GNY_CONFIG_FILE holds all of NYA_CONFIG, with "engine" and "game" at the top level like GNY_Config,
   * since gny_world_create loads it in one call. NYA_ConfigEngine's fields are one level down; the next
   * test loads NYA_ConfigEngine alone from a fixture shaped for it.
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
     * Driven rather than waited on, and more than one frame by design: nya_asset_get stats at most once
     * per interval, and reload waits for the timestamp to settle. Same as test_i18n_reload.c.
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

    // watching the same path again repoints the watch, as a game does after a code reload moved its global.
    u32              watch_count = nya_app_get()->config_system.watch_count;
    NYA_ConfigEngine moved       = { 0 };
    NYA_EXPECT(nya_config_watch(FIXTURE_PATH, nya_reflect_of(NYA_ConfigEngine), &moved));

    nya_assert(moved.physics.sub_steps == 8, "the new instance is loaded, got %u", moved.physics.sub_steps);
    nya_assert(nya_app_get()->config_system.watch_count == watch_count, "no second watch for one path");

    write_fixture("nya 2 0\n"
                  "{\n"
                  "    renderer: object {\n"
                  "        shadow_bias: f32 0.0015;\n"
                  "        shadow_cascades: u32 3;\n"
                  "        shadow_map_size: u32 1024;\n"
                  "    };\n"
                  "    physics: object {\n"
                  "        gravity: f32 9.81;\n"
                  "        sub_steps: u32 2;\n"
                  "    };\n"
                  "}\n");

    reloaded = false;

    for (u32 frame = 0; frame < 40 && !reloaded; frame++) {
      SDL_Delay(20);
      end_frame();

      reloaded = moved.physics.sub_steps == 2;
    }

    nya_assert(reloaded, "the edit lands in the new instance, got %u", moved.physics.sub_steps);
    nya_assert(engine.physics.sub_steps == 8, "and the old instance is no longer written, got %u", engine.physics.sub_steps);

    printf("  PASSED\n");
  }

  printf("PASSED: test_runtime_config\n");

  return 0;
}
