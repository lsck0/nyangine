/**
 * @file wasm_ui.c
 *
 * The client-side-rendered slice: the same immediate-mode UI component the ui_ssr example serves from a
 * server, compiled to WebAssembly and driven entirely in the browser. No round trip — the component, its
 * state, the input pass and the HTML presenter all run in wasm. `web/ui.html` calls the two exports
 * below, mounts the returned HTML into a surface, and forwards a click straight back into wasm.
 *
 * This is the CSR counterpart of wasm_demo.c's headless serialize proof: where that demo showed the
 * arena → object → JSON chain runs in wasm, this shows the ui → ui_present_html → input chain does, so
 * one component is a client-side web app with the browser as just another presenter.
 *
 * WHAT COMPILES HERE, AND WHY THE HEADERS COME WHOLE BUT THE CODE DOES NOT
 *
 * The UI reads all of its state through nya_app_get(), which returns NYA_App* — and NYA_App (core_app.h)
 * embeds the renderer, asset, window, physics and world systems *by value*. So the type cannot be
 * defined without those systems' headers, which is why this file includes nyangine.h whole (with the
 * vendored SDL3/box2d/ufbx headers on the include line, as FLAGS_WASM_UI passes) rather than the
 * NYA_NO_SDL slice wasm_demo.c takes. The headers are only declarations; nothing GPU, physics or SDL is
 * *compiled* here.
 *
 * What is compiled is a hand-picked set of leaf translation units, never a module's unity file: os_wasm.c
 * (page/time/random), the base leaves the arena → string → object chain reaches, the math leaves the
 * layout measures in, the three core systems the UI actually needs — callbacks, events and input — and
 * the ui module with the HTML presenter. The renderer, asset, window, settings and audio .c files are
 * *not* compiled, so their SDL/GPU/box2d calls never reach the linker. The two core files that do carry
 * a little SDL (core_event.c's mutex and event-pump, core_input.c's IME and clipboard) have that SDL
 * gated off under OS_WASM beside the engine, so the native build stays byte-identical. The few window
 * functions the UI calls are answered by a tiny wasm backend at the bottom of this file, the way
 * os_wasm.c answers the os interfaces — a wasm build has exactly one windowless surface.
 * */

#include <emscripten/emscripten.h>
#include <stdio.h>
#include <string.h>

// NOT NYA_NO_SDL: see the file comment. The full header graph is what defines NYA_App, and the vendored
// SDL3/box2d/ufbx headers resolve it on the include line. NYA_HEADLESS keeps any GPU-device path in the
// headers compiled out, matching how a test build runs the engine without a device.
#define NYA_HEADLESS
#include "nyangine/nyangine.h"

// The os backend first, exactly as wasm_demo.c does: page/time/random for a module with no OS under it.
#include "nyangine/os/os_wasm.c"

// ── base: the arena → string → object → reflection leaves the UI and its systems reach ──
#include "nyangine/base/base_arena.c"
#include "nyangine/base/base_backtrace.c"
#include "nyangine/base/base_ceiling.c"
#include "nyangine/base/base_clock.c"
#include "nyangine/base/base_error.c"
#include "nyangine/base/base_hash.c"
#include "nyangine/base/base_logging.c"
#include "nyangine/base/base_object.c"
#include "nyangine/base/base_reflection.c"
#include "nyangine/base/base_string.c"
#include "nyangine/base/base_types.c"

// ── math: the vectors and shapes the layout computes in. math_matrix.c is left out: its constructors are
// overloaded on f16 and f32, which are the same type once base_types.h widens f16 to float on wasm, so it
// will not compile there — and the UI's layout is all rectangles and 2-vectors, no matrices. ──
#include "nyangine/math/math_shapes.c"
#include "nyangine/math/math_tween.c" // nya_ease, which the ui animates hovers and panels with
#include "nyangine/math/math_vector.c"

// ── core: only the three systems the UI reads through — callbacks, events, input. Not settings, asset,
// window, audio, world or the app loop; those are SDL/GPU/box2d and the UI does not need them here. ──
#include "nyangine/core/core_callback.c"
#include "nyangine/core/core_event.c"
#include "nyangine/core/core_input.c"

// ── the ui module and the HTML presenter. The other presenters (cell, record, shape) are left out;
// ui_present_shape.c is also the one ui file that reaches the asset system, which is not compiled here. ──
#include "nyangine/ui/ui.c"
#include "nyangine/ui/ui_present.c"
#include "nyangine/ui/ui_present_html.c"
#include "nyangine/ui/ui_layout.c"
#include "nyangine/ui/ui_style.c"
#include "nyangine/ui/ui_text.c"
#include "nyangine/ui/ui_widgets.c"
#include "nyangine/ui/ui_window.c"
#include "nyangine/ui/ui_draw.c"
#include "nyangine/ui/ui_input.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * A WASM WINDOW AND APP BACKEND — the leaves core_app.c and core_window.c would supply, minus SDL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * core_app.c (nya_app_get, _NYA_APP_INSTANCE) and core_window.c (the window queries) are not compiled:
 * both are steeped in SDL — SDL_Init, the audio and video bring-up, window and swapchain creation. But a
 * handful of their symbols are all the UI reaches, so they are answered here for the one windowless
 * surface a CSR build has, the way os_wasm.c answers the os interfaces. This is a wasm backend, not a
 * stub: the app instance is real state the input system keys on, and the window queries return the true
 * numbers for a browser surface with no HiDPI scaling of its own.
 */

/** What nya_app_get returns: the single app instance, exactly as the header declares it (core_app.c's twin). */
NYA_App _NYA_APP_INSTANCE;

NYA_App* nya_app_get(void) {
    nya_assert(_NYA_APP_INSTANCE.initialized);
    return &_NYA_APP_INSTANCE;
}

/** The one window, keyed on by the input and ui systems. Center-anchored, 640×480, no real surface. */
static NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 640,
    .screen_height = 480,
};

/** Resolves the one handle to the one window; anything else is unknown, as on native. */
NYA_Window* nya_window_get(NYA_WindowHandle window) {
    if (window.index == WINDOW.handle.index && window.generation == WINDOW.handle.generation) return &WINDOW;
    return nullptr;
}

/** A browser surface reports no scaling of its own here; the layout is in CSS pixels, which the cell metric matches. */
f32 nya_window_display_scale(NYA_WindowHandle window) {
    nya_unused(window);
    return 1.0F;
}

f32 nya_window_pixel_density(NYA_WindowHandle window) {
    nya_unused(window);
    return 1.0F;
}

/** The whole surface is usable: there is no notch or system bar over a canvas. */
NYA_Rect nya_window_safe_area(NYA_WindowHandle window) {
    NYA_Window* target = nya_window_get(window);
    if (target == nullptr) return (NYA_Rect){ 0 };

    return (NYA_Rect){ .x = 0, .y = 0, .width = (s32)target->screen_width, .height = (s32)target->screen_height };
}

/*
 * The remaining leaves the compiled set reaches but whose defining files are not compiled: uptime (its
 * source is monotonic time, which os_wasm provides), settings (a real, if empty, view onto the app
 * instance), and the world, gamepad, tracing and shape-presenter accessors the ui and input code touch
 * but a CSR build has no use for. Each is the smallest honest answer for a browser: no world, no gamepad,
 * tracing off, and the HTML presenter standing in for the render2d shape presenter that has no wasm GPU.
 */

/** Uptime measured from the wasm clock rather than an app-start stamp core_app.c would keep. */
f64 nya_app_uptime_s(void) {
    return (f64)nya_os_time_monotonic_ns() / 1'000'000'000.0;
}

/** The real accessor's body, minus core_settings.c's SDL key-name code: a view onto the app instance. */
NYA_SettingsSystem* nya_settings(void) {
    return &nya_app_get()->settings_system;
}

/** A CSR build runs no entity world; the ui only ever guards world-dependent paths behind these. */
b8 nya_world_exists(void) {
    return false;
}

NYA_World* nya_world(void) {
    return nullptr;
}

/** No gamepad on a page: the count is zero, so the per-pad queries below are never reached, only linked. */
u32 nya_gamepad_count(void) {
    return 0;
}

NYA_GamepadId nya_gamepad_at(u32 index) {
    nya_unused(index);
    return NYA_GAMEPAD_NONE;
}

b8 nya_gamepad_button_pressed(NYA_GamepadId pad, NYA_GamepadButton button) {
    nya_unused(pad), nya_unused(button);
    return false;
}

b8 nya_gamepad_button_just_pressed(NYA_GamepadId pad, NYA_GamepadButton button) {
    nya_unused(pad), nya_unused(button);
    return false;
}

b8 nya_gamepad_button_just_released(NYA_GamepadId pad, NYA_GamepadButton button) {
    nya_unused(pad), nya_unused(button);
    return false;
}

b8 nya_gamepad_axis_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    nya_unused(pad), nya_unused(axis), nya_unused(threshold);
    return false;
}

b8 nya_gamepad_axis_just_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    nya_unused(pad), nya_unused(axis), nya_unused(threshold);
    return false;
}

b8 nya_gamepad_axis_just_released(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    nya_unused(pad), nya_unused(axis), nya_unused(threshold);
    return false;
}

/** Ends the input system's per-frame gamepad bookkeeping; with no gamepad there is nothing to roll over. */
void nya_system_gamepad_tick_end(void) {}

/** Tracing is a native profiling facility; a wasm build measures nothing, so a scope opens and closes empty. */
NYA_TraceScope _nya_trace_scope_begin(NYA_TraceFeature feature) {
    nya_unused(feature);
    return (NYA_TraceScope){ 0 };
}

void _nya_trace_scope_end(NYA_TraceScope* scope) {
    nya_unused(scope);
}

/**
 * The default presenter a UI context is born with, before nya_ui_presenter_set installs the real one. On
 * native that default is the render2d shape presenter; a wasm build has no render2d, and start() always
 * installs the HTML presenter over this immediately, so a zeroed placeholder is only ever held, never
 * drawn through.
 */
const NYA_UIPresenter* nya_ui_presenter_shape(void) {
    static const NYA_UIPresenter placeholder = { .name = "wasm-none" };
    return &placeholder;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE — the whole application, in wasm globals (this is CSR: no cookie, no server)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static NYA_UIHtml HTML;

/** One session's whole state, exactly the ui_ssr shape — but here it lives in the module, not a cookie. */
typedef struct {
    s32 count;
    u32 tab;
    b8  dark;
    f32 volume;
} AppState;

static AppState APP = { 0 };

/** Set once, the first time an export is called, so a page need not call an init export of its own. */
static b8 STARTED = false;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE COMPONENT — one function, every surface; the same shape ui_ssr draws
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static void component(NYA_Window* window, NYA_UIPass pass, AppState* app) {
    NYA_UI* ui = nya_ui_begin(window, pass);

    if (nya_ui_panel_begin(ui, "app", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(420), .title = "nyangine · csr" })) {
        static const NYA_ConstCString TABS[] = { "counter", "theme" };
        (void)nya_ui_tabs(ui, "tabs", TABS, nya_carray_length(TABS), &app->tab);

        if (app->tab == 0) {
            char line[64] = { 0 };
            (void)snprintf(line, sizeof(line), "count: %d", app->count);
            nya_ui_label(ui, line);

            if (nya_ui_panel_begin(ui, "buttons", (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
                if (nya_ui_button(ui, "-1")) app->count--;
                nya_ui_size(ui, nya_ui_grow(1));
                if (nya_ui_button(ui, "+1")) app->count++;
                nya_ui_size(ui, nya_ui_grow(1));
                if (nya_ui_button(ui, "reset")) app->count = 0;

                nya_ui_panel_end(ui);
            }
        } else {
            (void)nya_ui_toggle(ui, "dark mode", &app->dark);
            nya_ui_label(ui, app->dark ? "the theme is dark" : "the theme is light");

            (void)nya_ui_slider(ui, "volume", &app->volume, 0.0F, 1.0F, 0.0F);

            char vol[32] = { 0 };
            (void)snprintf(vol, sizeof(vol), "volume: %d%%", (s32)(app->volume * 100.0F));
            nya_ui_label(ui, vol);
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** Runs one input pass with the current input, then a draw pass into HTML, exactly as ui_ssr's render. */
static void render_into_html(void) {
    component(&WINDOW, NYA_UI_PASS_INPUT, &APP);

    nya_ui_html_reset(&HTML);
    component(&WINDOW, NYA_UI_PASS_DRAW, &APP);

    // End the frame the way a real one does, so this pass's presses do not linger into the next. Called
    // directly rather than dispatched through the event queue: the hook is all the input system needs,
    // and a CSR build runs no event pump between renders.
    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/**
 * A settled render: two passes, so the body reflects the final layout. This UI's auto-sized panels learn
 * their height from the children laid out inside them, and apply it on the *next* pass — one frame of lag
 * that a live GPU window never shows because it draws continuously. A single request/response cannot wait
 * for the next frame, so it takes the two passes here; the second leaves HTML->rects matching what the
 * body shows, which is what an injected click needs to land on the right widget.
 */
static void render_settled(void) {
    render_into_html();
    render_into_html();
}

/** Feeds a click at (x, y) as a real mouse would: move there, press, release, all in one input frame. */
static void inject_click(f32 x, f32 y) {
    NYA_Event move = {
        .type                 = NYA_EVENT_MOUSE_MOVED,
        .as_mouse_moved_event = { .window = WINDOW.handle, .x = x, .y = y },
    };
    nya_system_input_handle_event(&move);

    NYA_Event down = {
        .type                  = NYA_EVENT_MOUSE_BUTTON_DOWN,
        .as_mouse_button_event = { .window = WINDOW.handle, .is_down = true, .button = NYA_MOUSE_BUTTON_LEFT, .x = x, .y = y, .clicks = 1 },
    };
    nya_system_input_handle_event(&down);

    NYA_Event up = {
        .type                  = NYA_EVENT_MOUSE_BUTTON_UP,
        .as_mouse_button_event = { .window = WINDOW.handle, .is_down = false, .button = NYA_MOUSE_BUTTON_LEFT, .x = x, .y = y, .clicks = 1 },
    };
    nya_system_input_handle_event(&up);
}

/** Brings up the systems the UI reads through and installs the HTML presenter. Idempotent. */
static void start(void) {
    if (STARTED) return;
    STARTED = true;

    // The app instance the input system keys on, then the three systems the UI reads through. No window,
    // renderer, audio or asset system — the HTML presenter measures in a monospace cell and needs none.
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    nya_system_callback_init();
    (void)nya_system_events_init();
    nya_system_input_init();

    nya_ui_html_init(&HTML, NYA_UI_HTML_CELL);
    nya_ui_presenter_set(&WINDOW, nya_ui_html_presenter(&HTML));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE EXPORTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Runs one draw pass of the demo component through the HTML presenter and returns the body elements. The
 * returned pointer is into HTML's fixed buffer and stays valid until the next call, the same contract the
 * presenter's fragment has. JS reads it with cwrap('nyangine_ui_render', 'string', []).
 */
EMSCRIPTEN_KEEPALIVE
const char* nyangine_ui_render(void) {
    start();
    render_settled();
    return nya_ui_html_body(&HTML);
}

/**
 * Turns a click on element `wN` into a synthetic pointer over that widget's rectangle, runs the input
 * pass then a draw pass, and returns the new body. `event` is the DOM event name the page forwards
 * ("click"); an id that names no widget simply changes nothing and a consistent surface comes back.
 */
EMSCRIPTEN_KEEPALIVE
const char* nyangine_ui_event(const char* id, const char* event) {
    start();
    nya_unused(event);

    // Settle first, so HTML->rects is the table for the layout the click was made against — the same
    // reason ui_ssr's handler renders once at its top before aiming the pointer.
    render_settled();

    // The id is "wN"; N indexes that rectangle table and nothing else, so a bad one aims at no widget
    // rather than at anything it should not reach — the same discipline ui_ssr's handler keeps.
    if (id != nullptr && id[0] == 'w') {
        u64 index = 0;
        if (nya_type_parse(NYA_TYPE_U64, (const u8*)(id + 1), strlen(id + 1), &index)) {
            NYA_Rectf rect = { 0 };
            if (nya_ui_html_rect(&HTML, (u32)index, &rect)) {
                inject_click(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F);
            }
        }
    }

    // Consume the click and settle the layout it may have changed (a tab switch reshapes the tree).
    render_settled();
    return nya_ui_html_body(&HTML);
}
