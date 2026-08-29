/**
 * Fonts as values: the handle, the name registry, default resolution, and metrics.
 *
 * The property leaned on hardest is that NYA_FONT_NONE resolves to the default, because that is what
 * lets a UI pass a font through everything without every widget checking whether one was set.
 *
 * ⚠ **Metrics used to be untestable here, and are not any more.** Measuring answered zero headless,
 * so this file could only test bookkeeping; laying text out needs no GPU, so it now goes through the
 * same code the real renderer uses and the numbers are real. That costs this test an app instance and
 * an asset system, which is what the setup below is for — measuring resolves a face through it.
 *
 * FACE is the one font in the tree. FACE2 names a file that does not exist, deliberately: it is only
 * ever used as "a different font" for the equality and registry cases, never measured.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE  "./assets/fonts/Aldrich.ttf"
#define FACE2 "./assets/fonts/mono.ttf"

s32 main(void) {
    // No real audio device, ever: nya_system_asset_init brings up SDL_mixer, and on a machine without
    // a sound card ALSA leaks its configuration tree while failing to open one — which the leak
    // sanitizer then fails this test over. The same hint test_asset.c sets, for the same reason.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    /*
     * Measuring resolves the face through the asset system, which is what makes a font a font rather
     * than a path and a number.
     *
     * The callback and event systems come with it: loading is performed by the frame-ended hook the
     * asset system registers, so without them a queued face would stay queued forever.
     */
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    nya_font_clear();

    // ── Construction and validity.
    {
        NYA_Font font = nya_font(FACE, 16.0F);

        nya_check(nya_font_valid(font), "a path and a positive size is a font");
        nya_check(font.point_size == 16.0F, "and it keeps its size");

        nya_check(!nya_font_valid(NYA_FONT_NONE), "a zeroed font is not valid");
        nya_check(!nya_font_valid(nya_font(nullptr, 16.0F)), "a null path is not");
        nya_check(!nya_font_valid(nya_font("", 16.0F)), "nor an empty one");
        nya_check(!nya_font_valid(nya_font(FACE, 0.0F)), "nor a zero size");
        nya_check(!nya_font_valid(nya_font(FACE, -3.0F)), "nor a negative one");
    }

    // ── Equality is by face and size, and compares the path's text rather than its pointer.
    {
        char copy[64];
        (void)snprintf(copy, sizeof(copy), "%s", FACE);

        nya_check(nya_font_equals(nya_font(FACE, 16.0F), nya_font(FACE, 16.0F)), "the same pair is the same font");
        nya_check(nya_font_equals(nya_font(FACE, 16.0F), nya_font(copy, 16.0F)),
                  "identical text in a different buffer is still the same font");
        nya_check(!nya_font_equals(nya_font(FACE, 16.0F), nya_font(FACE, 18.0F)), "a different size is a different font");
        nya_check(!nya_font_equals(nya_font(FACE, 16.0F), nya_font(FACE2, 16.0F)), "and so is a different face");
        nya_check(nya_font_equals(NYA_FONT_NONE, NYA_FONT_NONE), "two nothings are equal");
        nya_check(!nya_font_equals(NYA_FONT_NONE, nya_font(FACE, 16.0F)), "nothing is not something");
    }

    // ── The default, and NYA_FONT_NONE resolving to it.
    {
        nya_check(!nya_font_valid(nya_font_default()), "there is no default to begin with");
        nya_check(!nya_font_valid(nya_font_resolve(NYA_FONT_NONE)), "so nothing resolves to nothing");

        NYA_Font ui = nya_font(FACE, 16.0F);
        nya_font_default_set(ui);

        nya_check(nya_font_equals(nya_font_default(), ui), "the default should read back");
        nya_check(nya_font_equals(nya_font_resolve(NYA_FONT_NONE), ui), "and NONE should resolve to it");

        // A valid font must pass through untouched, or the default would override every explicit choice.
        NYA_Font title = nya_font(FACE, 48.0F);
        nya_check(nya_font_equals(nya_font_resolve(title), title), "an explicit font must not be replaced by the default");
    }

    // ── The registry.
    {
        nya_font_clear();
        nya_check(nya_font_count() == 0, "clear should empty it");

        nya_check(nya_font_register("ui", FACE, 16.0F), "registering should work");
        nya_check(nya_font_register("title", FACE, 48.0F), "twice, under different names");
        nya_check(nya_font_count() == 2, "and both should be counted, got %u", nya_font_count());

        nya_check(nya_font_registered("ui"), "the name should be found");
        nya_check(nya_font_equals(nya_font_named("ui"), nya_font(FACE, 16.0F)), "and give back what was registered");
        nya_check(nya_font_equals(nya_font_named("title"), nya_font(FACE, 48.0F)), "for each name");

        // Looked up by text, not pointer: two call sites naming "ui" hold two different literals.
        char name[8];
        (void)snprintf(name, sizeof(name), "ui");
        nya_check(nya_font_registered(name), "a name in another buffer should still be found");

        nya_check(!nya_font_registered("nope"), "an unregistered name is not found");
        nya_check(!nya_font_valid(nya_font_named("nope")), "and gives back nothing");
        nya_check(!nya_font_valid(nya_font_named(nullptr)), "a null name gives back nothing");
    }

    // ── An invalid registration is refused rather than stored.
    {
        u32 before = nya_font_count();

        nya_check(!nya_font_register("bad", nullptr, 16.0F), "a null path must be refused");
        nya_check(!nya_font_register("bad", FACE, 0.0F), "a zero size must be refused");
        nya_check(!nya_font_register(nullptr, FACE, 16.0F), "a null name must be refused");
        nya_check(!nya_font_register("", FACE, 16.0F), "an empty name must be refused");

        nya_check(nya_font_count() == before, "none of that should have registered anything");
        nya_check(!nya_font_registered("bad"), "and the name must not exist");
    }

    // ── Re-registering replaces rather than accumulating.
    {
        u32 before = nya_font_count();

        nya_check(nya_font_register("ui", FACE2, 22.0F), "re-registering should succeed");
        nya_check(nya_font_count() == before, "and not add a second entry");
        nya_check(nya_font_equals(nya_font_named("ui"), nya_font(FACE2, 22.0F)), "the new font should be in force");
    }

    // ── Unregistering.
    {
        u32 before = nya_font_count();

        nya_font_unregister("ui");
        nya_check(!nya_font_registered("ui"), "it should be gone");
        nya_check(nya_font_count() == before - 1, "and the count should drop");

        nya_font_unregister("never registered");
        nya_font_unregister(nullptr);
        nya_check(nya_font_count() == before - 1, "removing what is not there changes nothing");
    }

    // ── The table refuses overflow rather than overwriting an entry.
    {
        nya_font_clear();

        static char names[NYA_FONT_REGISTRY_MAX + 4][16];
        u32         registered = 0;

        for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX + 4; i++) {
            (void)snprintf(names[i], sizeof(names[i]), "font_%u", i);
            if (nya_font_register(names[i], FACE, 8.0F + (f32)i)) registered++;
        }

        nya_check(registered == NYA_FONT_REGISTRY_MAX, "exactly the table's worth should register, got %u", registered);
        nya_check(nya_font_count() == NYA_FONT_REGISTRY_MAX, "and the count should agree");

        // The first entry must still be intact — overflow must not have recycled a slot.
        nya_check(nya_font_equals(nya_font_named("font_0"), nya_font(FACE, 8.0F)), "the first entry must survive overflow");
    }

    // ── The degenerate cases, which a UI reaches constantly.
    {
        nya_font_clear();

        nya_check(nya_font_width(NYA_FONT_NONE, "text") == 0.0F, "no default and no font measures zero");
        nya_check(nya_font_height(NYA_FONT_NONE, "text") == 0.0F, "in both axes");

        nya_font_default_set(nya_font(FACE, 16.0F));
        nya_check(nya_font_width(NYA_FONT_NONE, nullptr) == 0.0F, "null text measures zero");

        // Must not crash with no window; headless drawing is a no-op.
        nya_font_draw(nullptr, NYA_FONT_NONE, nullptr, 0.0F, 0.0F, NYA_COLOR_WHITE);

        nya_font_clear();
    }

    /*
     * ── Real metrics and real measurement, headless.
     *
     * The face is loaded asynchronously, so the first ask queues it and answers zero — which is
     * correct and is what every caller already copes with. Pumped until it lands rather than asserted
     * on the first call.
     */
    {
        NYA_Font ui = nya_font(FACE, 24.0F);

        f32 width = 0.0F;
        for (u32 attempt = 0; attempt < 8 && width <= 0.0F; attempt++) {
            // The load is queued by the first ask and performed by the frame-ended hook, so a frame
            // has to end between asking and being answered.
            width = nya_font_width(ui, "Hello");
            nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        }

        nya_check(width > 0.0F, "a loaded face should measure a real width, got %f", (f64)width);

        NYA_FontMetrics metrics = nya_font_metrics(ui);
        nya_check(metrics.line_height > 0.0F, "a line height, got %f", (f64)metrics.line_height);
        nya_check(metrics.ascent > 0.0F, "a positive ascent");
        nya_check(metrics.descent > 0.0F, "and a descent reported positive");
        nya_check(fabsf(metrics.height - (metrics.ascent + metrics.descent)) < 0.001F, "height should be ascent plus descent");

        // A longer string is wider, which is the cheapest check that this is measurement and not a
        // constant. Two lines are taller than one, likewise.
        nya_check(nya_font_width(ui, "Hello there") > width, "a longer string should be wider");
        nya_check(nya_font_height(ui, "one\ntwo") > nya_font_height(ui, "one"), "two lines should be taller than one");

        /*
         * Measuring a named font must not leave it current.
         *
         * nya_font_metrics reads through the renderer's current-font state, so it makes its argument
         * current for the duration and restores it afterwards — a HUD that measures a title font once
         * must not start drawing in it.
         */
        nya_font_default_set(ui);
        (void)nya_font_metrics(nya_font(FACE, 48.0F));

        nya_check(nya_render2d_font_size_get() == 24.0F, "measuring another size must restore the current font, got %f",
                  (f64)nya_render2d_font_size_get());

        nya_font_clear();
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
