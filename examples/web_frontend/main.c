/**
 * @file examples/web_frontend/main.c
 *
 * A real web frontend: a todo list built out of the `nya_ui_*` widgets, served by an `http_server`, with
 * the browser as just another presenter. The same immediate-mode `component` below is what a desktop
 * window or a terminal would draw — here it is rendered to HTML on the server and driven live, the way
 * `examples/ui_ssr` is, but the list it shows is not a counter in a cookie: it is rows in a database the
 * server keeps, read and written as the person clicks. That is the difference this example exists to show
 * — a UI toolkit talking to a backend store, not a static page.
 *
 * ```
 * ./build run example web_frontend           # serves on http://127.0.0.1:47830
 * ./web_frontend.example --port 8080
 * ```
 *
 * Open `http://127.0.0.1:47830/`, type a todo and press "add", tick items done, delete them, and switch
 * the filter. Stop the server and start it again and the list is still there, because it lives in
 * `web_frontend.db` under the save root, not in memory. Two browsers see the same list — it is the
 * server's, not the session's — while the half-typed draft and the chosen filter are per browser, carried
 * in a sealed cookie exactly as `ui_ssr` carries its counter.
 *
 * ## The loop, and where the store comes in
 *
 * This is `ui_ssr`'s loop with a database wired into the middle of it:
 *
 * 1. `GET /` reads the todos from the database, runs the component through the HTML presenter, and
 *    returns the whole page.
 * 2. a click POSTs `{ id, event }` to `/event`; a keystroke in the field POSTs `{ id, event, value }`.
 * 3. the server re-renders the layout the click was made against, aims a synthetic pointer at the named
 *    widget, and runs the component's input pass — the same pass a real mouse feeds. The component does
 *    not touch the database; it records what the click *meant* (add, toggle, delete, clear) into a small
 *    context, so the component stays a pure description of a UI that could just as well run in a browser.
 * 4. the handler applies that one intent to the database — an ORM insert, update or delete — reloads the
 *    rows, runs the draw pass, and returns the new HTML, which the client swaps in.
 *
 * Step 3 is not special-cased per widget: a click becomes a pointer over a rectangle, and the component's
 * own button and toggle logic decide what a click there means, just as in `ui_ssr`. The new part is step
 * 4 — the intent the component recorded is turned into a row change, so the immediate-mode UI is a real
 * front end against `http_server` and its `db` store.
 *
 * ## The same component, client-side (CSR)
 *
 * The `component` function below reads and writes only its `Ctx` argument — it makes no HTTP or database
 * call of its own — which is exactly what lets the *same* function be compiled to WebAssembly and run in
 * the browser with no server round trip, the way `src/web/wasm_ui.c` compiles `ui_ssr`'s component today.
 * That file is the working CSR harness: it includes the hand-picked leaf translation units the UI needs
 * (the arena → object chain, the math the layout measures in, the callback/event/input systems and the
 * `ui` module with the HTML presenter), answers the few window/app symbols a windowless surface needs,
 * and exports `nyangine_ui_render()` and `nyangine_ui_event(id, event)` that `web/ui.html` calls. Build it
 * with `./build wasm-ui` and serve `web/`.
 *
 * To ship *this* component as CSR you would do the same three things, and only these:
 *
 *   1. Compile this `component` (and its `Ctx`/`Todo` types) into a wasm translation unit shaped like
 *      `src/web/wasm_ui.c` — the include list there is the whole recipe and does not change.
 *   2. Where the SSR path calls `load_todos` to fill `Ctx` from the database, the CSR module has no
 *      database under it: it fills `Ctx` from a JSON list the page `fetch()`ed from this server. This
 *      example serves exactly that list at `GET /api/todos` (see below) so the wasm client has a real
 *      endpoint to read, and the browser posts an add/toggle/delete back to a small JSON API.
 *   3. Mount the returned HTML and forward clicks into the module's exports, byte for byte the JS in
 *      `web/ui.html`.
 *
 * So the server-rendered app here and the client-rendered app share one `component`; SSR renders it on the
 * server against the database directly, CSR renders it in wasm against the same list fetched over HTTP.
 * Wiring a second wasm target into the build is deliberately left out of this pass — `wasm_ui.c` already
 * proves the mechanism, and duplicating it here would be a build-system change, not a UI one — but the
 * `GET /api/todos` route below is the concrete seam a CSR build reads through, and it is live now.
 *
 * ## Security
 *
 * Every label the presenter emits is HTML-escaped, so a todo whose text is `<script>` is shown as that
 * text and never becomes markup. Each value the browser sends crosses into the database as a bound ORM
 * parameter, so a todo of `'); DROP TABLE todos;--` is stored and handed back as that string. The `/event`
 * id is `wN`, only ever an index into *this* server's last render, so a bad id aims a pointer at nothing.
 * The page carries a per-response CSP nonce for its one inline script, the same policy `ui_ssr` sets. The
 * database file is not encrypted — see the note in `db.h` — so nothing goes in it that would matter on
 * disk.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

/* THE STORE — todos are rows in a database, so a restart keeps them */

#define DEFAULT_PORT 47830

/** How long the loop sleeps between ticks; the sockets are the listener thread's. */
#define TICK_SLEEP_MS 5

/** Todos held at once. A fixed ceiling, as everything in this engine has. */
#define TODOS_MAX 128

/** Longest todo kept, terminator included. */
#define TODO_TEXT_MAX 256

/** Longest draft the field carries in the cookie. */
#define DRAFT_MAX 128

/** One todo. This is the Model — what a row is — and it never leaves this file as itself. */
typedef struct {
    s64  id;
    char text[TODO_TEXT_MAX];
    s64  done;
    f64  created_at_s;
} Todo;

/*
 * The description the ORM builds the table from, written by hand for the reason web_server's file spells
 * out: the reflection pass scans src/nyangine and src/gnyame, so a type declared in an example has no
 * generated table and nya_reflect_of would not resolve it. Inside the engine these declarations are one
 * `// @reflect` comment.
 */
NYA_INTERNAL const NYA_TypeReflection TODO_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = TODO_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = TODO_TEXT_MAX,
};

NYA_INTERNAL const NYA_ReflectField TODO_FIELDS[] = {
    // @key: the database assigns it, because an id that is zero on the way in is one the row has not got yet.
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(Todo, id), .is_key = true },
    { .name = "text", .type = &TODO_TEXT_ARRAY, .offset = nya_offsetof(Todo, text) },
    { .name = "done", .type = nya_reflect_of(s64), .offset = nya_offsetof(Todo, done) },
    { .name = "created_at_s", .type = nya_reflect_of(f64), .offset = nya_offsetof(Todo, created_at_s) },
};

NYA_INTERNAL const NYA_TypeReflection TODO_MODEL = {
    .name        = "Todo",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(Todo),
    .alignment   = alignof(Todo),
    .fields      = TODO_FIELDS,
    .field_count = nya_carray_length(TODO_FIELDS),
};

/* The connection and the table outlive every request, so they are static; an exchange's arena is scratch. */
NYA_INTERNAL NYA_Arena*    TODOS_ARENA = nullptr;
NYA_INTERNAL NYA_Database* TODOS_DB    = nullptr;
NYA_INTERNAL NYA_OrmTable* TODOS_TABLE = nullptr;

/* STATE — the UI systems, and the per-session bits that ride in a sealed cookie */

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

/** A windowless window: the UI keys its per-window tables on a NYA_Window, not on a real surface. */
NYA_INTERNAL NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 640,
    .screen_height = 480,
};

NYA_INTERNAL NYA_UIHtml HTML;

/** The key the cookie is sealed with, made at startup: a restart forgets each session's draft and filter,
 *  which is fine because the todos themselves are in the database, not the cookie. See http_seal.h. */
NYA_INTERNAL u8 SEAL_KEY[32] = { 0 };

#define STATE_COOKIE "nya_frontend"
#define STATE_LABEL  "web_frontend.state"
#define STATE_TTL_S  (7 * 24 * 3600)

/** The filter tabs. The list is the server's; which slice of it this browser looks at is the session's. */
typedef enum {
    FILTER_ALL    = 0,
    FILTER_ACTIVE = 1,
    FILTER_DONE   = 2,
} Filter;

/**
 * One session's UI state, carried in a sealed cookie — not the todos, which are shared and in the
 * database, but the transient things a browser is in the middle of: the draft it is typing and the filter
 * it has chosen. Two browsers get two of these; the server keeps none of it per client.
 * */
typedef struct {
    char draft[DRAFT_MAX];
    u32  filter;
} AppState;

/* THE COMPONENT — one function, every surface. It reads and writes only Ctx, so it also compiles to wasm. */

/** What a click in the UI meant, recorded by the component and applied to the store by the handler. */
typedef enum {
    ACTION_NONE = 0,
    ACTION_ADD,
    ACTION_TOGGLE,
    ACTION_DELETE,
    ACTION_CLEAR_DONE,
} ActionKind;

/**
 * Everything the component draws from and writes back to, in one place. The todos are loaded from the
 * database before a pass and are read-only to the component; the component's only output beyond the UI
 * itself is `action` — the one intent a click carried — which the handler turns into a row change. This
 * indirection is what keeps `component` free of any HTTP or database call, and so compilable to wasm.
 * */
typedef struct {
    AppState*   app;
    const Todo* todos;
    u32         todo_count;

    ActionKind action;
    s64        action_id;   // the todo an action names, for toggle and delete.
    b8         action_done; // the value a toggle settled on, so the handler writes exactly that.
} Ctx;

/** True when a todo passes the current filter and should be drawn. */
NYA_INTERNAL b8 todo_visible(const Todo* todo, u32 filter) {
    switch ((Filter)filter) {
        case FILTER_ACTIVE: return todo->done == 0;
        case FILTER_DONE: return todo->done != 0;
        case FILTER_ALL:
        default: return true;
    }
}

/**
 * The todo list, described the same way for a browser, a terminal and a GPU. On an input pass a click
 * lands on one widget and records an intent into `ctx`; on a draw pass each widget is handed to whatever
 * presenter is installed. The list it walks is `ctx->todos`, filled from the database by the caller.
 * */
NYA_INTERNAL void component(NYA_Window* window, NYA_UIPass pass, Ctx* ctx) {
    NYA_UI*   ui  = nya_ui_begin(window, pass);
    AppState* app = ctx->app;

    if (nya_ui_panel_begin(ui, "app", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(460), .title = "nyangine · todo" })) {
        // The filter, one of three chosen. Which slice this browser sees is its own; the rows are shared.
        static const NYA_ConstCString FILTERS[] = { "all", "active", "done" };
        (void)nya_ui_tabs(ui, "filter", FILTERS, nya_carray_length(FILTERS), &app->filter);

        // The composer: a field bound to the session's draft, and an add button beside it. Typing posts a
        // "text" write-back that the server types into this same buffer; pressing add records ACTION_ADD.
        if (nya_ui_panel_begin(ui, "compose", (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
            (void)nya_ui_text_input(ui, "new todo", app->draft, sizeof(app->draft));
            nya_ui_size(ui, nya_ui_grow(1));
            if (nya_ui_button(ui, "add")) ctx->action = ACTION_ADD;

            nya_ui_panel_end(ui);
        }

        u32 shown = 0;
        u32 done  = 0;

        // One row per todo that passes the filter. Each row is its own panel, so the toggle and the delete
        // button inside it are scoped to that row: the constant labels "done" and "delete" never collide
        // across rows, whatever the todo text is. A click the server injected lands on exactly one of them.
        for (u32 i = 0; i < ctx->todo_count; i++) {
            const Todo* todo = &ctx->todos[i];

            if (todo->done != 0) done++;
            if (!todo_visible(todo, app->filter)) continue;

            shown++;

            char row_id[32] = { 0 };
            (void)snprintf(row_id, sizeof(row_id), "row%lld", (long long)todo->id);

            if (nya_ui_panel_begin(ui, row_id, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
                b8 checked = todo->done != 0;

                // The toggle owns the done state. On an input pass a click flips `checked` and the toggle
                // returns true; the component records which todo and the value it settled on, and the
                // handler writes exactly that to the row — no read-modify-write guessing later.
                if (nya_ui_toggle(ui, "done", &checked)) {
                    ctx->action      = ACTION_TOGGLE;
                    ctx->action_id   = todo->id;
                    ctx->action_done = checked;
                }

                nya_ui_label(ui, todo->text);
                nya_ui_size(ui, nya_ui_grow(1));

                if (nya_ui_button(ui, "delete")) {
                    ctx->action    = ACTION_DELETE;
                    ctx->action_id = todo->id;
                }

                nya_ui_panel_end(ui);
            }
        }

        char summary[96] = { 0 };
        (void)snprintf(summary, sizeof(summary), "%u shown · %u done · %u total", shown, done, ctx->todo_count);
        nya_ui_label(ui, summary);

        if (done > 0 && nya_ui_button(ui, "clear done")) ctx->action = ACTION_CLEAR_DONE;

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/* THE STORE, READ AND WRITTEN */

/** Loads every todo, oldest first, into `ctx` from the exchange's arena. A failed read shows an empty list. */
NYA_INTERNAL void load_todos(Ctx* ctx, NYA_Arena* arena) {
    void* rows  = nullptr;
    u32   count = 0;

    NYA_Error selected = nya_orm_select(TODOS_TABLE, arena, "ORDER BY id", nullptr, 0, &rows, &count);

    if (!selected.ok || count > TODOS_MAX) {
        ctx->todos      = nullptr;
        ctx->todo_count = 0;
        return;
    }

    // The rows come back contiguous, `type->size` apart; nya_orm_at is that arithmetic, so the array is a
    // plain Todo* the component can index.
    ctx->todos      = (const Todo*)rows;
    ctx->todo_count = count;
}

/** Applies the one intent a click recorded to the database. Returns after the row is written. */
NYA_INTERNAL void apply_action(Ctx* ctx, NYA_Arena* arena) {
    AppState* app = ctx->app;

    switch (ctx->action) {
        case ACTION_ADD: {
            // Nothing to add, a full store, and a draft that is only whitespace are all the caller's
            // business, not a fault: the draft simply stays put and no row is written.
            if (app->draft[0] == '\0' || ctx->todo_count >= TODOS_MAX) break;

            Todo todo = { .done = 0, .created_at_s = nya_app_uptime_s() };
            (void)snprintf(todo.text, sizeof(todo.text), "%s", app->draft);

            // The id is left zero, so the database assigns it and nya_orm_insert writes it back. The text
            // is bound as a parameter, whatever quotes and semicolons the draft held.
            if (nya_orm_insert(TODOS_TABLE, &todo).ok) app->draft[0] = '\0';
            break;
        }

        case ACTION_TOGGLE: {
            Todo todo = { 0 };
            if (!nya_orm_find(TODOS_TABLE, arena, nya_sql_s64(ctx->action_id), &todo).ok) break;

            todo.done = ctx->action_done ? 1 : 0;
            (void)nya_orm_update(TODOS_TABLE, &todo);
            break;
        }

        case ACTION_DELETE:
            // A row that is already gone is not an error here; the list simply comes back without it.
            (void)nya_orm_delete(TODOS_TABLE, nya_sql_s64(ctx->action_id));
            break;

        case ACTION_CLEAR_DONE: {
            void* rows  = nullptr;
            u32   count = 0;

            if (!nya_orm_select(TODOS_TABLE, arena, "WHERE done <> 0", nullptr, 0, &rows, &count).ok) break;

            for (u32 i = 0; i < count; i++) {
                const Todo* todo = nya_orm_at(TODOS_TABLE, rows, i);
                (void)nya_orm_delete(TODOS_TABLE, nya_sql_s64(todo->id));
            }
            break;
        }

        case ACTION_NONE:
        default:
            break;
    }
}

/* THE LOOP — render, and turn an event into an input pass (the ui_ssr shape) */

/** One input pass with whatever has been injected, then a draw pass into HTML, then the frame's end. */
NYA_INTERNAL void render(Ctx* ctx) {
    component(&WINDOW, NYA_UI_PASS_INPUT, ctx);

    nya_ui_html_reset(&HTML);
    component(&WINDOW, NYA_UI_PASS_DRAW, ctx);

    // The frame ends the way a real one does, so this pass's presses do not linger into the next.
    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/**
 * A settled render: two passes, so the body reflects the final layout. The auto-sized panels learn their
 * height from the children laid out inside them and apply it on the next pass — one frame of lag a live
 * window never shows because it draws continuously, but a single response has to wait for. The list here
 * changes length as rows are added and removed, so the settle matters more than it does for ui_ssr.
 * */
NYA_INTERNAL void render_settled(Ctx* ctx) {
    render(ctx);
    render(ctx);
}

/** One input pass alone, for the multi-step field edit below; draws nothing. */
NYA_INTERNAL void input_pass(Ctx* ctx) {
    component(&WINDOW, NYA_UI_PASS_INPUT, ctx);

    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/** Feeds a click at (x, y) as a real mouse would: move there, press, release, all in one input frame. */
NYA_INTERNAL void inject_click(f32 x, f32 y) {
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

/**
 * Sets the field under `box` to `value`, the string the browser now shows, through the input system — the
 * same idea ui_ssr uses. A click focuses the field, ctrl+A selects the whole line, and the value replaces
 * the selection; an empty value deletes it. Each step is its own input frame, because focus and the caret
 * only settle between passes. When it returns, the field's own editing has written `value` into the draft.
 * */
NYA_INTERNAL void inject_field_text(Ctx* ctx, NYA_Rectf box, NYA_ConstCString value) {
    inject_click(box.x + (box.width * 0.5F), box.y + (box.height * 0.5F));
    input_pass(ctx);

    inject_key(NYA_KEY_A, true, NYA_KEYMOD_CTRL);
    input_pass(ctx);

    inject_key(NYA_KEY_A, false, NYA_KEYMOD_NONE);

    if (value != nullptr && value[0] != '\0') {
        inject_text(value);
    } else {
        inject_key(NYA_KEY_DELETE, true, NYA_KEYMOD_NONE);
    }

    input_pass(ctx);
}

/* SEALED-COOKIE SESSION STATE */

/** Reads a session's UI state out of its sealed cookie, or a fresh zeroed one when there is no valid cookie. */
NYA_INTERNAL AppState app_from_cookie(NYA_HttpExchange* exchange) {
    AppState app = { 0 };

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, STATE_COOKIE, &cookie)) return app;

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (cookie.size >= sizeof(token)) return app;
    nya_memcpy(token, cookie.text, cookie.size);

    // A tampered, expired or forged cookie opens to nothing and the session starts fresh; the seal is what
    // makes trusting a client-held blob safe.
    u8  bytes[sizeof(AppState)] = { 0 };
    u64 size                    = 0;

    if (nya_http_unseal(SEAL_KEY, sizeof(SEAL_KEY), STATE_LABEL, token, strlen(token), bytes, sizeof(bytes), &size) && size == sizeof(AppState)) {
        nya_memcpy(&app, bytes, sizeof(AppState));
    }

    // A filter that a bad cookie put out of range is clamped, so the component's switch always has a case.
    if (app.filter > FILTER_DONE) app.filter = FILTER_ALL;

    return app;
}

/** Seals a session's UI state back into its cookie, so the next request from that browser carries it. */
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

/* HANDLERS */

/** The first load: the whole page, so a browser has the surface, the stylesheet and the client. */
NYA_INTERNAL NYA_HttpStatus handle_page(NYA_HttpExchange* exchange) {
    AppState app = app_from_cookie(exchange);
    Ctx      ctx = { .app = &app };

    load_todos(&ctx, exchange->arena);
    render_settled(&ctx);

    if (!app_to_cookie(exchange, &app)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // A per-response nonce for the one inline script, so the page's CSP can allow that script by nonce and
    // nothing else. style-src 'unsafe-inline' is the one loosening, safe because the presenter escapes
    // every label; the rest is the tight default. This is ui_ssr's policy, verbatim.
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

    static char page[NYA_UI_HTML_MAX + 8192];
    u32         written = nya_ui_html_document(&HTML, page, sizeof(page), "nyangine · todo", nonce);

    if (written == 0) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)page, written, NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** An event from the page: aim a pointer at the widget it names, run the component, apply what the click
 *  meant to the store, and answer with the new body. */
NYA_INTERNAL NYA_HttpStatus handle_event(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id_value    = nya_object_get(body, "id");
    NYA_Value* event_value = nya_object_get(body, "event");
    if (id_value == nullptr || id_value->type != NYA_TYPE_STRING) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_ConstCString event = event_value != nullptr && event_value->type == NYA_TYPE_STRING ? event_value->as_string : "click";

    // The id is "wN"; the number indexes the last render's rectangles and nothing else, so a bad one aims
    // at no widget rather than at anything it should not reach.
    NYA_ConstCString text = id_value->as_string;
    if (text[0] != 'w') return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 id = 0;
    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)(text + 1), strlen(text + 1), &id)) return NYA_HTTP_STATUS_BAD_REQUEST;

    // This session's UI state and the current rows, then a render so the id-to-rectangle table matches the
    // page the click was made against — the previous render may have been another browser's.
    AppState app = app_from_cookie(exchange);
    Ctx      ctx = { .app = &app };

    load_todos(&ctx, exchange->arena);
    render(&ctx);

    NYA_UIWidgetKind kind       = NYA_UI_WIDGET_LABEL;
    NYA_Rectf        value_rect = { 0 };
    NYA_Rectf        rect       = { 0 };

    if (nya_string_equals(event, "text") && nya_ui_html_widget(&HTML, (u32)id, &kind, &value_rect) && kind == NYA_UI_WIDGET_FIELD) {
        // The field's value is the whole string the browser now shows; typing it into the field over its
        // own contents is what makes the draft match the browser, and it goes in through the input system
        // rather than by touching the buffer directly. No row changes — typing is not adding.
        NYA_ConstCString value = "";
        NYA_Value*       v     = nya_object_get(body, "value");
        if (v != nullptr && v->type == NYA_TYPE_STRING) value = v->as_string;

        inject_field_text(&ctx, value_rect, value);
        render_settled(&ctx);
    } else if (nya_ui_html_rect(&HTML, (u32)id, &rect)) {
        // A plain click: aim the pointer at the widget's centre and run the input pass, which records the
        // intent into ctx. Reset the intent first so only this frame's click counts.
        ctx.action = ACTION_NONE;
        inject_click(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F);
        render(&ctx);

        // Turn the intent into a row change, then reload and settle so the body shows the new list.
        apply_action(&ctx, exchange->arena);
        load_todos(&ctx, exchange->arena);
        render_settled(&ctx);
    } else {
        // An unknown id changes nothing; the client still gets a consistent surface back.
        render_settled(&ctx);
    }

    if (!app_to_cookie(exchange, &app)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_ConstCString fragment = nya_ui_html_body(&HTML);

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)fragment, (u64)strlen(fragment), NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * The list as JSON. The SSR pages above never call this — they read the database directly — but it is the
 * seam a client-rendered build of the same component reads through: a wasm module has no database under
 * it, so it fetches this and fills its Ctx from the array. See the CSR note at the top of the file.
 * */
NYA_INTERNAL NYA_HttpStatus handle_api_todos(NYA_HttpExchange* exchange) {
    void* rows  = nullptr;
    u32   count = 0;

    if (!nya_orm_select(TODOS_TABLE, exchange->arena, "ORDER BY id", nullptr, 0, &rows, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object*           out   = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* todos = nya_array_create(exchange->arena, NYA_Value);

    for (u32 i = 0; i < count; i++) {
        const Todo* todo = nya_orm_at(TODOS_TABLE, rows, i);

        NYA_Object* item = nya_object_create(exchange->arena);
        nya_object_add(item, "id", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = todo->id });
        nya_object_add(item, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)todo->text });
        nya_object_add(item, "done", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = todo->done != 0 });

        NYA_Value value = { .type = NYA_TYPE_OBJECT, .as_object = *item };
        nya_array_push_back(todos, value);
    }

    nya_object_add(out, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = todos->length });
    nya_object_add(out, "todos", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *todos });

    return nya_http_response_json(exchange->response, exchange->arena, out).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* ROUTES AND MAIN */

/*
 * Every route is NYA_HTTP_AFFINITY_MAIN and the server runs no workers: the UI and the input system are
 * the ticking thread's, and the ORM table is one connection with no lock around it, so both are touched
 * where this program's loop is and nowhere else. The server is drained from the loop below.
 */
NYA_INTERNAL const NYA_HttpRoute ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = "/", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_page,
      .summary = "The todo page", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/event", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_event,
      .summary = "One interaction, applied to the store and answered with the new HTML",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_GET, .path = "/api/todos", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_api_todos,
      .summary = "The todo list as JSON — the seam a client-rendered build reads through",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

NYA_INTERNAL const NYA_HttpRouter ROUTER = {
    .name = "todo", .routes = ROUTES, .route_count = nya_carray_length(ROUTES),
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

    // The systems the UI reads through, and the save root the database lives under. No window, renderer,
    // world or audio: the HTML presenter measures in a monospace cell and needs none of them. This is the
    // ui_ssr bring-up plus web_server's save root.
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    nya_system_settings_init();
    nya_system_callback_init();
    defer nya_system_callback_deinit();
    NYA_EXPECT(nya_system_events_init(), "while starting the event registry");
    defer nya_system_events_deinit();
    nya_system_input_init();
    defer nya_system_input_deinit();
    // No window opens, but a field taking focus starts text input, which looks the window handle up; the
    // system has to be up for that lookup to resolve to "no such window" rather than read an unallocated table.
    nya_system_window_init();
    defer nya_system_window_deinit();
    nya_system_asset_init();
    defer nya_system_asset_deinit();

    // The save root, where the database lands. Fatal if there is none: a server whose whole job is to keep
    // what it is sent cannot run without somewhere to keep it.
    NYA_Error saves = nya_system_save_init();
    if (!saves.ok) {
        nya_log_error("No save root, so there is nowhere to keep the todos: %s", (NYA_ConstCString)saves.message);
        return EXIT_FAILURE;
    }
    defer nya_system_save_deinit();

    TODOS_ARENA = nya_arena_create(.name = "todos_db");
    defer       nya_arena_destroy(TODOS_ARENA);

    NYA_Error stored = nya_save_database_open(TODOS_ARENA, "web_frontend.db", &TODOS_DB);
    if (!stored.ok) {
        nya_log_error("Could not open the todo database: %s", (NYA_ConstCString)stored.message);
        return EXIT_FAILURE;
    }
    defer nya_sql_close(TODOS_DB);

    NYA_EXPECT(nya_orm_open(TODOS_ARENA, TODOS_DB, &TODO_MODEL, "todos", &TODOS_TABLE), "while binding the todo model to its table");
    defer nya_orm_close(TODOS_TABLE);

    // Creates the table on a first run, and on any later one brings it level with the struct above.
    NYA_EXPECT(nya_orm_schema_migrate(TODOS_TABLE), "while bringing the todos table level with the model");

    if (!nya_os_random_bytes(SEAL_KEY, sizeof(SEAL_KEY))) {
        nya_log_error("The system random source failed, so no state-sealing key could be made.");
        return EXIT_FAILURE;
    }

    nya_ui_html_init(&HTML, NYA_UI_HTML_CELL);
    nya_ui_presenter_set(&WINDOW, nya_ui_html_presenter(&HTML));

    // Single-threaded: the UI, the input system and the one database connection are the ticking thread's,
    // so every route is MAIN and the server is drained from the loop below rather than by workers.
    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = 0 }), "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&ROUTER), "while mounting the todo routes");

    nya_log_info("web_frontend on http://127.0.0.1:%u — open it and add a todo. ctrl-c to stop.", nya_http_server_port());
    nya_log_info("The list is at /api/todos as JSON, and in web_frontend.db under the save root — a restart keeps it.");

    while (RUNNING) {
        nya_system_http_tick();
        SDL_Delay(TICK_SLEEP_MS);
    }

    nya_log_info("Stopping.");
    nya_ui_html_deinit(&HTML);

    return EXIT_SUCCESS;
}
