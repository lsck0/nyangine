/**
 * The distance-field mode survives being asked for before the face exists.
 *
 * This is a regression test for a bug that shipped working-looking code: `nya_font_sdf_set` needed a
 * loaded `TTF_Font`, answered false and forgot when there was none, and there never is one at the point
 * a game registers its fonts — the asset system resolves a face over the frames *after* it is first
 * named. So the one call a game would naturally write did nothing at all, and the title font it was
 * meant to switch stayed a bitmap. Nothing caught it because the call's return value was ignored, which
 * is exactly what a `(void)`-ed setter invites.
 *
 * What is asserted is the ordering that matters: the request outlives the frames before the face
 * arrives, and by the time anything could bake an atlas from that face the mode is on it. An atlas is
 * sized from the face's metrics and flagged with the mode it was baked in, and a distance field has
 * larger metrics than coverage — so applying the mode late is not "late", it is wrong.
 *
 * Headless. The asset system's loading pass runs on NYA_EVENT_FRAME_ENDED and a test can dispatch that
 * by hand, which is what test_asset.c does and why a font can be brought all the way up with no GPU.
 * The atlas itself needs a device and is out of reach here; see the findings on the glyph atlas.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A font that is actually in the repository, so the face has something valid to parse. */
#define FACE "./assets/fonts/Aldrich.ttf"

#define POINT_SIZE 24.0F

/** Drains both queues, the way the end of a real frame does. */
static void end_frame(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

/** Runs frames until `font`'s face has resolved, or gives up. Returns the face, or null. */
static TTF_Font* pump_until_loaded(NYA_Font font) {
    for (u32 i = 0; i < 16; i++) {
        TTF_Font* face = nya_text_font_for(font.path, font.point_size);
        if (face != nullptr) return face;

        end_frame();
    }

    return nullptr;
}

s32 main(void) {
    // No real audio device: nya_system_asset_init brings up SDL_mixer, and on a machine without a sound
    // card ALSA leaks its configuration tree while failing to open one. Same reason as test_asset.c.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_assert(TTF_Init(), "TTF_Init failed: %s", SDL_GetError());
    defer TTF_Quit();

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    /*
     * ── ⭐ Asking before the face exists is accepted, and lands once it does.
     *
     * The whole bug, in the order a game actually writes it: register, ask, draw later.
     */
    {
        nya_font_clear();

        nya_check(nya_font_register("title", FACE, POINT_SIZE), "the font should register");

        NYA_Font title = nya_font_named("title");

        // Nothing has resolved the face yet, which is the precondition the old implementation failed on.
        nya_check(nya_text_font_for(FACE, POINT_SIZE) == nullptr, "the face should not be loaded yet, or this tests nothing");

        nya_check(nya_font_sdf_set(title, true), "asking before the face exists must be accepted, not refused");

        // And reported as what it is going to be, rather than as false.
        nya_check(nya_font_sdf(title), "the mode a caller just asked for should read back as set");

        TTF_Font* face = pump_until_loaded(title);
        nya_check(face != nullptr, "the face should have loaded within a few frames");

        // The face is the authority once it exists, and this is the assertion the bug failed: the
        // request has to have been pushed onto it without the caller asking a second time.
        nya_check(TTF_GetFontSDF(face), "the distance field must be on the face by the time it is usable");
        nya_check(nya_font_sdf(title), "and reported through the font API");
    }

    /*
     * ── The mode is on the face before anything can measure or draw through it.
     *
     * Ordering, not just eventual arrival. render2d bakes an atlas sized from the face's metrics the
     * first time a glyph is drawn, so a mode applied after the first measurement is a mode applied
     * after the metrics it changes have already been read.
     */
    {
        nya_font_clear();

        nya_check(nya_font_register("title", FACE, POINT_SIZE), "the font should register");

        NYA_Font title = nya_font_named("title");

        nya_check(nya_font_sdf_set(title, true), "asking should be accepted");

        // Frames pass with nobody touching the font, exactly as they do while a game sits on a menu.
        for (u32 i = 0; i < 8; i++) end_frame();

        // The first thing to reach the face is a measurement, which is what layer_ui.c does before it
        // draws. It has to find the mode already applied.
        f32x2 measured = nya_font_measure(title, "AVATAR");

        TTF_Font* face = nya_text_font_for(FACE, POINT_SIZE);
        nya_check(face != nullptr, "the measurement should have resolved the face");
        nya_check(TTF_GetFontSDF(face), "measuring must not be able to observe the face before the mode is on it");

        nya_check(measured.x > 0.0F && measured.y > 0.0F, "and the measurement should still be a real one, got %fx%f",
                  (f64)measured.x, (f64)measured.y);
    }

    // ── Turning it back off is pushed too, not just remembered.
    {
        nya_font_clear();

        nya_check(nya_font_register("body", FACE, POINT_SIZE), "the font should register");

        NYA_Font body = nya_font_named("body");

        nya_check(nya_font_sdf_set(body, true), "on should be accepted");

        TTF_Font* face = pump_until_loaded(body);
        nya_check(face != nullptr && TTF_GetFontSDF(face), "on should have landed");

        nya_check(nya_font_sdf_set(body, false), "off should be accepted");
        nya_check(!TTF_GetFontSDF(face), "and pushed onto the face it was already applied to, not skipped as unchanged");
        nya_check(!nya_font_sdf(body), "and reported off");
    }

    // ── A font nobody asked about is left alone.
    {
        nya_font_clear();

        nya_check(nya_font_register("plain", FACE, POINT_SIZE), "the font should register");

        NYA_Font plain = nya_font_named("plain");

        TTF_Font* face = pump_until_loaded(plain);
        nya_check(face != nullptr, "the face should have loaded");

        // Coverage, not a field. The request table is opt-in, so a font with no request must not pick
        // one up from a font that shares nothing but a registry.
        nya_check(!nya_font_sdf(plain), "a font nobody asked about must not be rasterising as a distance field");
    }

    /*
     * ── Clearing the registry clears the requests with it.
     *
     * They are keyed by path and size rather than by name, so nothing else would ever drop one — and a
     * request outliving its registry would silently reapply itself to the next font that happened to
     * share a path and a size, which is precisely what every font in a small game does.
     */
    {
        nya_font_clear();

        nya_check(nya_font_register("title", FACE, POINT_SIZE), "the font should register");
        nya_check(nya_font_sdf_set(nya_font_named("title"), true), "asking should be accepted");

        nya_font_clear();

        nya_check(nya_font_register("body", FACE, POINT_SIZE), "re-registering the same face should work");

        NYA_Font body = nya_font_named("body");

        // The face may well still be loaded from the block above, so this reads the face itself rather
        // than a pending answer — which is the stricter of the two.
        TTF_Font* face = nya_text_font_for(FACE, POINT_SIZE);

        if (face != nullptr) {
            // Whatever the previous request left on the face is not this font's business, but nothing
            // should be re-pushing it either. Asked for explicitly so the state is unambiguous.
            nya_check(nya_font_sdf_set(body, false), "setting it off should be accepted");
            nya_check(!TTF_GetFontSDF(face), "and take effect");
        }

        nya_check(!nya_font_sdf(body), "a cleared request must not survive into the next registration");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
