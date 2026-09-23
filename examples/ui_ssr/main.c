/**
 * @file examples/ui_ssr/main.c
 *
 * The same immediate-mode UI component, served to a browser and live — a click in the page runs the
 * component on the server and the changed HTML comes back. Phoenix LiveView, in C, with no front-end.
 *
 * ```
 * ./build run example ui_ssr            # serves on http://127.0.0.1:47820
 * ```
 *
 * Open `http://127.0.0.1:47820/` and press the buttons. Nothing runs in the browser but a few lines of
 * script that forward a click and swap in the HTML that comes back; the counter, the theme and every
 * decision live on the server, in the one `component` function below — the exact function that would
 * draw to a GPU window or a terminal.
 *
 * ## The loop
 *
 * 1. `GET /` runs the component through the HTML presenter and returns the whole page.
 * 2. a click on a widget POSTs `{ id, event }` to `/event`.
 * 3. the server looks up where that widget was drawn, aims a synthetic pointer at it, and runs the
 *    component's input pass — the same pass a real mouse feeds. The button under the pointer activates,
 *    the state changes.
 * 4. the server runs the draw pass again and returns the new elements, which the client swaps in.
 *
 * The point is that step 3 is not special-cased per widget. A click becomes a pointer over a rectangle,
 * and the component's own logic — the same logic the desktop build runs — decides what a click there
 * means. One component, and the browser is just another presenter.
 *
 * ## What this demonstrates and what it does not
 *
 * It is click-driven: buttons and a toggle, which a synthetic pointer drives generically. Text fields
 * and sliders carry a value the client would send too, and wiring that back is the same idea with the
 * value set instead of a pointer synthesised; it is left out here to keep the loop one honest mechanism
 * rather than a table of special cases. There is also no diffing yet — the whole surface is replaced on
 * each event rather than the one element that changed — which a real deployment adds so a patch is a few
 * bytes; the ids are already stable for exactly that.
 *
 * ## Security
 *
 * Every label is HTML-escaped by the presenter, so a name a person types cannot become markup. The
 * event body is small JSON with a widget id and an event name, and the id is only ever used to look up a
 * rectangle from *this* server's last render — a client that sends a bad id gets a pointer aimed at
 * nothing, not a way to reach anything it should not. The server holds all the state; the browser holds
 * none.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE — the whole application, on the server
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define DEFAULT_PORT 47820

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

/** A windowless window: the UI needs a NYA_Window to key its per-window tables on, not a real one. */
NYA_INTERNAL NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 640,
    .screen_height = 480,
};

NYA_INTERNAL NYA_UIHtml HTML;

/** The application's entire state. A click changes one of these and the page redraws from it. */
NYA_INTERNAL s32 STATE_COUNT   = 0;
NYA_INTERNAL b8  STATE_DARK     = false;
NYA_INTERNAL u32 STATE_TAB      = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE COMPONENT — one function, every surface
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The UI, described the same way for a browser, a terminal and a GPU. It reads the state above and, on
 * an input pass, changes it; on a draw pass it hands each widget to whatever presenter is installed.
 * */
NYA_INTERNAL void component(NYA_Window* window, NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(window, pass);

    if (nya_ui_panel_begin(ui, "app", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(420), .title = "nyangine · live" })) {
        // a row of tabs, one chosen. Clicking one is a click the server turns into a pointer.
        static const NYA_ConstCString TABS[] = { "counter", "theme" };
        (void)nya_ui_tabs(ui, "tabs", TABS, nya_carray_length(TABS), &STATE_TAB);

        if (STATE_TAB == 0) {
            char line[64] = { 0 };
            (void)snprintf(line, sizeof(line), "count: %d", STATE_COUNT);
            nya_ui_label(ui, line);

            if (nya_ui_panel_begin(ui, "buttons", (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
                if (nya_ui_button(ui, "-1")) STATE_COUNT--;
                nya_ui_size(ui, nya_ui_grow(1));
                if (nya_ui_button(ui, "+1")) STATE_COUNT++;
                nya_ui_size(ui, nya_ui_grow(1));
                if (nya_ui_button(ui, "reset")) STATE_COUNT = 0;

                nya_ui_panel_end(ui);
            }
        } else {
            (void)nya_ui_toggle(ui, "dark mode", &STATE_DARK);
            nya_ui_label(ui, STATE_DARK ? "the theme is dark" : "the theme is light");
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE LOOP — render, and turn an event into an input pass
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Runs one input pass with the current input, then a draw pass into HTML. Leaves HTML holding the body. */
NYA_INTERNAL void render(void) {
    component(&WINDOW, NYA_UI_PASS_INPUT);

    nya_ui_html_reset(&HTML);
    component(&WINDOW, NYA_UI_PASS_DRAW);

    // The frame ends the way a real one does, so this pass's presses do not linger into the next.
    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/** Feeds a click at (x, y) as a real mouse would: move there, press, release, all in one input frame. */
NYA_INTERNAL void inject_click(f32 x, f32 y) {
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The first load: the whole page, so a browser has the surface, the stylesheet and the client. */
NYA_INTERNAL NYA_HttpStatus handle_page(NYA_HttpExchange* exchange) {
    render();

    /*
     * A per-response nonce for the one inline script, so the page's own Content-Security-Policy can allow
     * that script by nonce and nothing else. The server default is `default-src 'none'`, which would block
     * the presenter's inline style and script outright; this route sets its own, tighter where it can be:
     * the script is pinned to this nonce, and `style-src 'unsafe-inline'` is the one loosening — safe here
     * because the presenter escapes every label, so no attribute a person set can carry style of its own.
     */
    u8 nonce_bytes[16] = { 0 };
    if (!nya_os_random_bytes(nonce_bytes, sizeof(nonce_bytes))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    char nonce[32] = { 0 };
    u64  nonce_len = 0;
    if (!nya_crypto_base64url_encode(nonce_bytes, sizeof(nonce_bytes), nonce, sizeof(nonce), &nonce_len)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    char policy[256] = { 0 };
    (void)snprintf(policy, sizeof(policy),
                   "default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-%s'; connect-src 'self'; base-uri 'none'; form-action 'none'",
                   nonce);

    if (!nya_http_response_header(exchange->response, "Content-Security-Policy", policy).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    static char page[NYA_UI_HTML_MAX + 8192];
    u32         written = nya_ui_html_document(&HTML, page, sizeof(page), "nyangine · live", nonce);

    if (written == 0) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)page, written, NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** An event from the page: aim a pointer at the widget it names, run the component, answer the new body. */
NYA_INTERNAL NYA_HttpStatus handle_event(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id_value = nya_object_get(body, "id");
    if (id_value == nullptr || id_value->type != NYA_TYPE_STRING) return NYA_HTTP_STATUS_BAD_REQUEST;

    // The id is "wN"; the number is an index into the last render's rectangles and nothing else, so a
    // bad one aims at no widget rather than at anything it should not reach.
    NYA_ConstCString text = id_value->as_string;
    if (text[0] != 'w') return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 id = 0;
    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)(text + 1), strlen(text + 1), &id)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Rectf rect = { 0 };
    if (nya_ui_html_rect(&HTML, (u32)id, &rect)) {
        inject_click(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F);
    }

    // Whether or not the id resolved, re-render: an unknown id simply changes nothing, and the client
    // still gets a consistent surface back.
    render();

    NYA_ConstCString fragment = nya_ui_html_body(&HTML);

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)fragment, (u64)strlen(fragment), NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROUTES AND MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const NYA_HttpRoute ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = "/", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_page,
      .summary = "The live page", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/event", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_event,
      .summary = "One interaction, answered with the new HTML",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

NYA_INTERNAL const NYA_HttpRouter ROUTER = {
    .name = "ui", .routes = ROUTES, .route_count = nya_carray_length(ROUTES),
};

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") != 0) continue;
        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
            nya_log_error("--port expects a number, got '%s'.", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();

    // The systems the UI reads through, and nothing to do with a window: an app instance, the callback
    // and event registries, the input system the injected clicks go into, and the asset system a style
    // may reach for. The same set the UI tests bring up.
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    nya_system_settings_init();
    nya_system_callback_init();
    defer nya_system_callback_deinit();
    NYA_EXPECT(nya_system_events_init(), "while starting the event registry");
    defer nya_system_events_deinit();
    nya_system_input_init();
    defer nya_system_input_deinit();
    nya_system_asset_init();
    defer nya_system_asset_deinit();

    nya_ui_html_init(&HTML, NYA_UI_HTML_CELL);
    nya_ui_presenter_set(&WINDOW, nya_ui_html_presenter(&HTML));

    // Single-threaded: the UI and the input system are the ticking thread's, so every route is MAIN and
    // the server is drained from the loop below rather than by workers.
    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = 0 }), "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&ROUTER), "while mounting the UI routes");

    nya_log_info("ui_ssr on http://127.0.0.1:%u — open it and press the buttons. ctrl-c to stop.", nya_http_server_port());

    while (RUNNING) {
        nya_system_http_tick();
        nya_os_time_sleep_ms(2);
    }

    nya_log_info("Stopping.");
    nya_ui_html_deinit(&HTML);

    return EXIT_SUCCESS;
}
