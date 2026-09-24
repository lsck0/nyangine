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
 * A click on a button or a toggle is driven by a synthetic pointer, generically. A slider and a text
 * field carry a value the client sends too, and that is wired the same way: the slider's value aims a
 * drag along its track, and the field's value is typed into it over a select-all — both go in through
 * the input system, so the component's own slider and field logic decide what the value means, not a
 * special case per widget. There is no diffing yet — the whole surface is replaced on each event rather
 * than the one element that changed — which a real deployment adds so a patch is a few bytes; the ids
 * are already stable for exactly that.
 *
 * The value each session holds — the counter, the theme, the volume, the typed name — round-trips in the
 * sealed cookie, so it is per browser. Focus and the caret are transient UI state the server process
 * keeps for its one window, not per session; sharing them across browsers is the same limitation the
 * slider's drag has, and does not affect the value, which is what the cookie carries.
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

/* STATE — the whole application, on the server */

#define DEFAULT_PORT 47820

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

/** A windowless window: the UI needs a NYA_Window to key its per-window tables on, not a real one. */
NYA_INTERNAL NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 640,
    .screen_height = 480,
};

NYA_INTERNAL NYA_UIHtml HTML;

/**
 * One session's whole state — the counter, the theme, the tab — carried in a sealed cookie, not on the
 * server. Two browsers get two of these, because each request brings its own; the server keeps nothing
 * per client. See http_seal.h: sealed so a client cannot read or forge it, and it fits a cookie many
 * times over at twelve bytes.
 * */
typedef struct {
    s32  count;
    u32  tab;
    b8   dark;
    f32  volume;
    char name[48];
} AppState;

/** The key the state cookie is sealed with, made at startup: a restart forgets every session, which for
 *  a counter is fine and for a real app is where a keyring persisted across restarts would go. */
NYA_INTERNAL u8 SEAL_KEY[32] = { 0 };

/** The cookie the sealed state travels in. A __Host- cookie over TLS; plain here since the demo is http. */
#define STATE_COOKIE "nya_state"
#define STATE_LABEL  "ui_ssr.state"
#define STATE_TTL_S  (7 * 24 * 3600)

/* THE COMPONENT — one function, every surface */

/**
 * The UI, described the same way for a browser, a terminal and a GPU. It reads the state above and, on
 * an input pass, changes it; on a draw pass it hands each widget to whatever presenter is installed.
 * */
NYA_INTERNAL void component(NYA_Window* window, NYA_UIPass pass, AppState* app) {
    NYA_UI* ui = nya_ui_begin(window, pass);

    if (nya_ui_panel_begin(ui, "app", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(420), .title = "nyangine · live" })) {
        // a row of tabs, one chosen. Clicking one is a click the server turns into a pointer.
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

            // a text field, driven live from the browser: what a person types there is sent as a "text"
            // event, and the server writes it into this buffer through the field's own editing — the same
            // path a keyboard drives. See handle_event.
            (void)nya_ui_text_input(ui, "name", app->name, sizeof(app->name));

            char hello[96] = { 0 };
            (void)snprintf(hello, sizeof(hello), "hello, %s", app->name[0] != '\0' ? app->name : "stranger");
            nya_ui_label(ui, hello);
        } else {
            (void)nya_ui_toggle(ui, "dark mode", &app->dark);
            nya_ui_label(ui, app->dark ? "the theme is dark" : "the theme is light");

            // a slider, driven live from the browser: its input event carries a value the server turns
            // into a pointer along the track, which is what a real drag is. See handle_event.
            (void)nya_ui_slider(ui, "volume", &app->volume, 0.0F, 1.0F, 0.0F);

            char vol[32] = { 0 };
            (void)snprintf(vol, sizeof(vol), "volume: %d%%", (s32)(app->volume * 100.0F));
            nya_ui_label(ui, vol);
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/* THE LOOP — render, and turn an event into an input pass */

/** Runs one input pass with the current input, then a draw pass into HTML. Leaves HTML holding the body. */
NYA_INTERNAL void render(AppState* app) {
    component(&WINDOW, NYA_UI_PASS_INPUT, app);

    nya_ui_html_reset(&HTML);
    component(&WINDOW, NYA_UI_PASS_DRAW, app);

    // The frame ends the way a real one does, so this pass's presses do not linger into the next.
    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/** Reads a session's state out of its sealed cookie, or a fresh zeroed one when there is no valid cookie. */
NYA_INTERNAL AppState app_from_cookie(NYA_HttpExchange* exchange) {
    AppState app = { 0 };

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, STATE_COOKIE, &cookie)) return app;

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (cookie.size >= sizeof(token)) return app;
    nya_memcpy(token, cookie.text, cookie.size);

    // A tampered, expired or forged cookie simply opens to nothing and the session starts fresh; the
    // seal is what makes trusting a client-held blob safe. See http_seal.h.
    u8  bytes[sizeof(AppState)] = { 0 };
    u64 size                    = 0;

    if (nya_http_unseal(SEAL_KEY, sizeof(SEAL_KEY), STATE_LABEL, token, strlen(token), bytes, sizeof(bytes), &size) && size == sizeof(AppState)) {
        nya_memcpy(&app, bytes, sizeof(AppState));
    }

    return app;
}

/** Seals a session's state back into its cookie, so the next request from that browser carries it. */
NYA_INTERNAL b8 app_to_cookie(NYA_HttpExchange* exchange, const AppState* app) {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    if (!nya_http_seal(SEAL_KEY, sizeof(SEAL_KEY), STATE_LABEL, (const u8*)app, sizeof(AppState), STATE_TTL_S, token, sizeof(token)).ok) return false;

    return nya_http_response_cookie(exchange->response,
                                    &(NYA_HttpCookie){
                                        .name      = STATE_COOKIE,
                                        .value     = token,
                                        .max_age_s = STATE_TTL_S,
                                        .http_only = true,
                                        .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                    })
        .ok;
}

/** Presses at a point, moves to another while held, and releases: a drag, which is what a slider reads. */
NYA_INTERNAL void inject_drag(f32 x, f32 y) {
    NYA_Event move = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .window = WINDOW.handle, .x = x, .y = y } };
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

/** Presses or releases a key with the given modifiers, as a keyboard would. */
NYA_INTERNAL void inject_key(NYA_Keycode key, b8 down, NYA_KeyModFlag modifiers) {
    NYA_Event event = {
        .type         = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP,
        .as_key_event = { .window = WINDOW.handle, .is_down = down, .key = key, .modifier_flags = modifiers },
    };
    nya_system_input_handle_event(&event);
}

/** Commits typed text, as a keyboard or an IME would: the focused field inserts it at the caret. */
NYA_INTERNAL void inject_text(NYA_ConstCString text) {
    NYA_Event event = { .type = NYA_EVENT_TEXT_INPUT, .as_text_input_event = { .window = WINDOW.handle, .text = text } };
    nya_system_input_handle_event(&event);
}

/** One input pass over the component with whatever has been injected, then the frame's end. Draws nothing. */
NYA_INTERNAL void input_pass(AppState* app) {
    component(&WINDOW, NYA_UI_PASS_INPUT, app);

    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/**
 * Sets the field under `box` to `value`, the string a browser sent, through the input system — the same
 * idea as the slider, but text instead of a pointer. A click focuses the field, ctrl+A selects the whole
 * line, and the typed value replaces the selection; an empty value deletes it, clearing the field. Each
 * step is its own input frame, because focus and the caret only settle between passes, exactly as they do
 * for a person. When it returns, the component's own editing has written `value` into the caller's buffer.
 */
NYA_INTERNAL void inject_field_text(AppState* app, NYA_Rectf box, NYA_ConstCString value) {
    // focus: a click in the box starts the field editing.
    inject_click(box.x + (box.width * 0.5F), box.y + (box.height * 0.5F));
    input_pass(app);

    // select the whole line, so the value replaces the text rather than inserting into it.
    inject_key(NYA_KEY_A, true, NYA_KEYMOD_CTRL);
    input_pass(app);

    // release ctrl (so the next text is typed, not read as a shortcut), then type the value over the
    // selection — or, when it is empty, delete the selection to clear the field.
    inject_key(NYA_KEY_A, false, NYA_KEYMOD_NONE);

    if (value != nullptr && value[0] != '\0') {
        inject_text(value);
    } else {
        inject_key(NYA_KEY_DELETE, true, NYA_KEYMOD_NONE);
    }

    input_pass(app);
}

/* HANDLERS */

/* EMBEDDING METADATA — so a link to this page unfurls on social media and chat apps */

/**
 * The social-media embedding metadata for this page. The URL fields are built into the caller's buffers from
 * the port the server actually bound, so the canonical link and the oEmbed discovery URL point at this very
 * process. The literals describe the demo; a real app would fill these from its content.
 *
 * The same value backs both the `<head>` tags (nya_ui_html_document_meta) and the `/oembed` answer
 * (nya_ui_page_meta_oembed), which is the point of a single composable metadata value.
 * */
NYA_INTERNAL NYA_PageMeta page_meta(OUT char* canonical, u64 canonical_cap, OUT char* oembed, u64 oembed_cap) {
    u16 port = nya_http_server_port();

    (void)snprintf(canonical, canonical_cap, "http://127.0.0.1:%u/", port);
    (void)snprintf(oembed, oembed_cap, "http://127.0.0.1:%u/oembed?url=http%%3A%%2F%%2F127.0.0.1%%3A%u%%2F", port, port);

    return (NYA_PageMeta){
        .title         = "nyangine · live",
        .description   = "One immediate-mode UI component, served to a browser and live — Phoenix LiveView in C, no front-end.",
        .canonical_url = canonical,
        .image_url     = "https://raw.githubusercontent.com/nyangine/nyangine/master/docs/preview.png",
        .image_alt     = "The nyangine live UI demo",
        .site_name     = "nyangine",
        .author_name   = "nyangine",
        .type          = "website",
        .twitter_card  = NYA_TWITTER_CARD_SUMMARY_LARGE_IMAGE,
        .locale        = "en_US",
        .oembed_url    = oembed,
    };
}

/** The first load: the whole page, so a browser has the surface, the stylesheet and the client. */
NYA_INTERNAL NYA_HttpStatus handle_page(NYA_HttpExchange* exchange) {
    AppState app = app_from_cookie(exchange);
    render(&app);
    if (!app_to_cookie(exchange, &app)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

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
                   "default-src 'none'; img-src data:; style-src 'unsafe-inline'; script-src 'nonce-%s'; connect-src 'self'; base-uri 'none'; form-action 'none'",
                   nonce);

    if (!nya_http_response_header(exchange->response, "Content-Security-Policy", policy).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The embedding metadata, so a link to this page unfurls with a title, blurb and image on social media
    // and in chat apps, and carries the oEmbed discovery link. Every field is escaped into the head.
    char canonical[64] = { 0 };
    char oembed[160]   = { 0 };
    NYA_PageMeta meta  = page_meta(canonical, sizeof(canonical), oembed, sizeof(oembed));

    static char page[NYA_UI_HTML_MAX + 8192];
    u32         written = nya_ui_html_document_meta(&HTML, page, sizeof(page), "nyangine · live", nonce, &meta);

    if (written == 0) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)page, written, NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** An event from the page: aim a pointer at the widget it names, run the component, answer the new body. */
NYA_INTERNAL NYA_HttpStatus handle_event(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id_value    = nya_object_get(body, "id");
    NYA_Value* event_value = nya_object_get(body, "event");
    if (id_value == nullptr || id_value->type != NYA_TYPE_STRING) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_ConstCString event = event_value != nullptr && event_value->type == NYA_TYPE_STRING ? event_value->as_string : "click";

    // The id is "wN"; the number is an index into the last render's rectangles and nothing else, so a
    // bad one aims at no widget rather than at anything it should not reach.
    NYA_ConstCString text = id_value->as_string;
    if (text[0] != 'w') return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 id = 0;
    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)(text + 1), strlen(text + 1), &id)) return NYA_HTTP_STATUS_BAD_REQUEST;

    // This session's state, and a render so the id-to-rectangle table matches the cookie the click was
    // made against — the previous render may have been another browser's.
    AppState app = app_from_cookie(exchange);
    render(&app);

    NYA_UIWidgetKind kind       = NYA_UI_WIDGET_LABEL;
    NYA_Rectf        value_rect = { 0 };
    NYA_Rectf        rect       = { 0 };

    if (nya_string_equals(event, "input") && nya_ui_html_widget(&HTML, (u32)id, &kind, &value_rect) && kind == NYA_UI_WIDGET_SLIDER) {
        // The slider's value arrives 0..1000 (the range input's span); aim a drag at that fraction of
        // the track, and the component's own slider logic writes the bound value from where the pointer is.
        f64 value = 0.0;
        NYA_Value* v = nya_object_get(body, "value");
        if (v != nullptr && v->type == NYA_TYPE_STRING) value = strtod(v->as_string, nullptr);

        f32 t = (f32)nya_clamp(value / 1000.0, 0.0, 1.0);
        inject_drag(value_rect.x + value_rect.width * t, value_rect.y + value_rect.height * 0.5F);

        render(&app);
    } else if (nya_string_equals(event, "text") && nya_ui_html_widget(&HTML, (u32)id, &kind, &value_rect) && kind == NYA_UI_WIDGET_FIELD) {
        // The field's value is the whole string the browser now shows; the value_rect is its box. Typing it
        // into the field over its own contents is what makes the server's buffer match the browser's, and it
        // goes in through the input system rather than by touching the buffer directly. This runs its own
        // input passes, so the render below is only the draw the client gets back.
        NYA_ConstCString value = "";
        NYA_Value* v = nya_object_get(body, "value");
        if (v != nullptr && v->type == NYA_TYPE_STRING) value = v->as_string;

        inject_field_text(&app, value_rect, value);

        render(&app);
    } else if (nya_ui_html_rect(&HTML, (u32)id, &rect)) {
        inject_click(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F);

        render(&app);
    } else {
        // An unknown id simply changes nothing; the client still gets a consistent surface back.
        render(&app);
    }

    // The changed state is sealed back into the cookie.

    if (!app_to_cookie(exchange, &app)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_ConstCString fragment = nya_ui_html_body(&HTML);

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)fragment, (u64)strlen(fragment), NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * `GET /oembed?url=…`: the structured metadata a consumer that found the discovery `<link>` fetches, as an
 * oEmbed 1.0 JSON document. The `url` parameter is required and must be an http(s) URL — the same gate the
 * head's URLs pass — so a `javascript:` or garbage `url` is a 400 rather than something echoed back. The
 * document itself is built by the engine from the page's NYA_PageMeta and rendered as JSON through serde.
 * */
NYA_INTERNAL NYA_HttpStatus handle_oembed(NYA_HttpExchange* exchange) {
    // The URL the consumer wants metadata for. A real provider matches it against the pages it serves; here
    // one page is served, so a well-formed http(s) `url` is accepted and anything else is refused.
    char url[512] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "url", url, sizeof(url))) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!nya_ui_page_meta_url_ok(url)) return NYA_HTTP_STATUS_BAD_REQUEST;

    char         canonical[64] = { 0 };
    char         oembed[160]   = { 0 };
    NYA_PageMeta meta          = page_meta(canonical, sizeof(canonical), oembed, sizeof(oembed));

    NYA_Object* document = nullptr;
    if (!nya_ui_page_meta_oembed(exchange->arena, &meta, &document).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_json(exchange->response, exchange->arena, document);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* ROUTES AND MAIN */

NYA_INTERNAL const NYA_HttpRoute ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = "/", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_page,
      .summary = "The live page", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/event", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_event,
      .summary = "One interaction, answered with the new HTML",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_GET, .path = "/oembed", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_oembed,
      .summary = "oEmbed metadata for a link to this page",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR } },
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
    // No window opens, but a field taking focus starts text input, which looks the window handle up; the
    // system has to be up for that lookup to resolve to "no such window" rather than read an unallocated
    // table. The UI text tests bring it up for the same reason.
    nya_system_window_init();
    defer nya_system_window_deinit();
    nya_system_asset_init();
    defer nya_system_asset_deinit();

    if (!nya_os_random_bytes(SEAL_KEY, sizeof(SEAL_KEY))) {
        nya_log_error("The system random source failed, so no state-sealing key could be made.");
        return EXIT_FAILURE;
    }

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
