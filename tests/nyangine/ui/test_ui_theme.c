/**
 * UI theme files: a NYA_UIStyle loaded from a .nya file through reflection, validated field by field
 * like a settings file, applied to a window, and — in a hot reload build — kept in sync with the file.
 **/

// Before the engine, so the watch and the reload tick are compiled in. See test_runtime_config.c.
#ifndef NYA_ASSET_HOT_RELOAD
#define NYA_ASSET_HOT_RELOAD
#endif

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

/** A path of its own under assets/ui, so no file this test is not responsible for is touched. */
#define FIXTURE_PATH "./assets/ui/__test_ui_theme.nya"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/** Rewrites the fixture until its timestamp moves, so a watch comparing timestamps sees the edit. See test_runtime_config.c. */
static void write_fixture(NYA_ConstCString text) {
  u64 before = 0;
  b8  existed = nya_filesystem_last_modified(FIXTURE_PATH, &before).ok;

  NYA_EXPECT(nya_file_write(FIXTURE_PATH, text));

  for (u32 attempt = 0; existed && attempt < 200; attempt++) {
    u64 after = 0;
    NYA_EXPECT(nya_filesystem_last_modified(FIXTURE_PATH, &after));
    if (after != before) break;

    SDL_Delay(5);
    NYA_EXPECT(nya_file_write(FIXTURE_PATH, text));
  }
}

/** One frame's worth of the end-of-frame work, which is where the reload pass lives. See test_runtime_config.c. */
static void end_frame(void) {
  nya_app_get()->frame_stats.uptime_ns = nya_clock_get_monotonic_ns();

  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

static b8 color_equals(NYA_Color a, NYA_Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  const NYA_UIStyle builtin = nya_ui_style_get(&window);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the shipped default theme loads and is the built-in look
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the shipped default theme loads into NYA_UIStyle as the built-in style\n");
  {
    NYA_EXPECT(nya_ui_theme_load(&window, NYA_UI_THEME_DEFAULT_FILE));

    NYA_UIStyle style = nya_ui_style_get(&window);

    nya_assert(style.body_size == NYA_UI_BODY_SIZE, "body_size, got %f", (f64)style.body_size);
    nya_assert(style.radius == NYA_UI_RADIUS, "radius, got %f", (f64)style.radius);
    nya_assert(style.focus_bar == NYA_UI_FOCUS_BAR, "focus_bar, got %f", (f64)style.focus_bar);
    nya_assert(color_equals(style.accent, NYA_UI_ACCENT), "accent should be the built-in accent");
    nya_assert(color_equals(style.panel, NYA_UI_PANEL), "panel should be the built-in panel");
    nya_assert(color_equals(style.text.disabled, NYA_UI_TEXT_DISABLED), "text.disabled should be the built-in colour");

    printf("  PASSED\n");
  }

  nya_ui_theme_clear(&window);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a valid theme overrides only the fields it names, the rest staying default
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a valid theme overrides the fields it names and defaults the rest\n");
  {
    write_fixture("nya 2 0\n"
                  "{\n"
                  "    radius: f32 12.0;\n"
                  "    padding: f32 20.0;\n"
                  "    accent: object { r: f32 0.9; g: f32 0.2; b: f32 0.4; a: f32 1.0; };\n"
                  "}\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_EXPECT(nya_ui_theme_load(&window, FIXTURE_PATH));

    NYA_UIStyle style = nya_ui_style_get(&window);

    nya_assert(style.radius == 12.0F, "the named radius, got %f", (f64)style.radius);
    nya_assert(style.padding == 20.0F, "the named padding, got %f", (f64)style.padding);
    nya_assert(color_equals(style.accent, (NYA_Color){ 0.9F, 0.2F, 0.4F, 1.0F }), "the named accent");

    // A field the file did not mention keeps its built-in default rather than zeroing.
    nya_assert(style.body_size == builtin.body_size, "an unnamed field stayed default, got %f", (f64)style.body_size);
    nya_assert(color_equals(style.panel, builtin.panel), "an unnamed colour stayed default");

    printf("  PASSED\n");
  }

  nya_ui_theme_clear(&window);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a bad field is rejected and the built-in default kept, valid siblings applied
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an out-of-range or wrong-type field is rejected, keeping the built-in default\n");
  {
    // A radius below zero, an alpha above one, a size that is text not a number, and a key that names no
    // field. The valid sibling (spacing) must still take, proving one bad line costs only its own field.
    write_fixture("nya 2 0\n"
                  "{\n"
                  "    radius: f32 -5.0;\n"
                  "    spacing: f32 9.0;\n"
                  "    accent: object { r: f32 0.3; g: f32 0.6; b: f32 0.9; a: f32 2.0; };\n"
                  "    body_size: string \"huge\";\n"
                  "    not_a_field: f32 1.0;\n"
                  "}\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_EXPECT(nya_ui_theme_load(&window, FIXTURE_PATH));

    NYA_UIStyle style = nya_ui_style_get(&window);

    nya_assert(style.radius == builtin.radius, "a negative radius fell back to the built-in, got %f", (f64)style.radius);
    nya_assert(color_equals(style.accent, builtin.accent), "an alpha above one rejected the whole colour");
    nya_assert(style.body_size == builtin.body_size, "a wrong-type size fell back to the built-in, got %f", (f64)style.body_size);
    nya_assert(style.spacing == 9.0F, "the valid sibling still applied, got %f", (f64)style.spacing);

    printf("  PASSED\n");
  }

  nya_ui_theme_clear(&window);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a resolved style round-trips through reflection unchanged
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: NYA_UIStyle round-trips through nya_reflect_to_object / from_object\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "theme_roundtrip");
    defer      nya_arena_destroy(arena);

    // A style with a spread of non-default numbers, so an accidental drop of any one shows.
    NYA_UIStyle original = builtin;
    original.radius      = 7.5F;
    original.padding     = 11.0F;
    original.title_size  = 41.0F;
    original.accent      = (NYA_Color){ 0.12F, 0.34F, 0.56F, 0.78F };
    original.text.pressed = (NYA_Color){ 0.9F, 0.8F, 0.7F, 1.0F };

    NYA_Object* object = nya_reflect_to_object(arena, nya_reflect_of(NYA_UIStyle), &original);
    nya_assert(object != nullptr, "to_object produced a document");

    NYA_UIStyle restored = { 0 };
    NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_UIStyle), &restored, object));

    nya_assert(restored.radius == original.radius, "radius survived, got %f", (f64)restored.radius);
    nya_assert(restored.padding == original.padding, "padding survived, got %f", (f64)restored.padding);
    nya_assert(restored.title_size == original.title_size, "title_size survived, got %f", (f64)restored.title_size);
    nya_assert(color_equals(restored.accent, original.accent), "accent survived the round-trip");
    nya_assert(color_equals(restored.text.pressed, original.text.pressed), "text.pressed survived the round-trip");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an edit to the theme file re-applies to the window live
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_ui_theme_load watches the file and re-applies an edit\n");
  {
    write_fixture("nya 2 0\n{\n    radius: f32 3.0;\n}\n");
    defer (void)remove(FIXTURE_PATH);

    NYA_EXPECT(nya_ui_theme_load(&window, FIXTURE_PATH));
    nya_assert(nya_ui_style_get(&window).radius == 3.0F, "the initial load, got %f", (f64)nya_ui_style_get(&window).radius);

    write_fixture("nya 2 0\n{\n    radius: f32 21.0;\n}\n");

    b8 reloaded = false;
    for (u32 frame = 0; frame < 40 && !reloaded; frame++) {
      SDL_Delay(20);
      end_frame();
      reloaded = nya_ui_style_get(&window).radius == 21.0F;
    }

    nya_assert(reloaded, "the edit should have been picked up, got %f", (f64)nya_ui_style_get(&window).radius);

    nya_ui_theme_clear(&window);

    printf("  PASSED\n");
  }

  printf("PASSED: test_ui_theme\n");

  return 0;
}
