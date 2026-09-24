/**
 * @file examples/secure_webapp/main.c
 *
 * A production-shaped web app that ties together the web-serving pieces the engine grew for shipping a
 * real site: a page that unfurls when its link is pasted, the well-known documents a crawler and an LLM
 * look for, gzip on the wire, a Prometheus scrape endpoint, and a members area behind a real account
 * login — every one of them a composed engine primitive rather than something written here.
 *
 * ```
 * ./build run example secure_webapp          # runs headless: serves on loopback, checks itself, exits 0
 * ./secure_webapp.example --serve            # serves on http://127.0.0.1:47830 until ctrl-c
 * ./secure_webapp.example --serve --port 8080
 * ```
 *
 * Run with no arguments it is a self test: it binds loopback, fetches a few of its own endpoints through
 * the HTTP client on a thread of its own while the main loop answers them, prints what it saw — the
 * `og:` tags on the home page, the Prometheus text, a gzip-encoded response — and exits. That is what
 * `./build run example secure_webapp` does, so the example proves itself every time it is built. `--serve`
 * is the other half: the same server, left running for a browser.
 *
 * With `--serve`, from another terminal (a cookie jar is how a browser keeps the session):
 *
 * ```
 * curl -i localhost:47830/                       # the home page: SSR HTML with OpenGraph/Twitter meta
 * curl -s --compressed -i localhost:47830/       # the same, gzip on the wire (Content-Encoding: gzip)
 * curl localhost:47830/oembed?url=https%3A%2F%2Fnyangine.example%2F   # the oEmbed the meta discovers
 * curl localhost:47830/sitemap.xml               # the pages worth crawling
 * curl localhost:47830/feed.xml                  # the feed, RSS 2.0 (and /atom.xml for Atom)
 * curl localhost:47830/robots.txt                # the crawl rules, /members and /api kept out
 * curl localhost:47830/llms.txt                  # the LLM guide, use restricted
 * curl localhost:47830/metrics                   # the Prometheus text exposition, for a scrape
 * curl -i localhost:47830/members                # 401: the members page is gated
 * curl -c jar -X POST localhost:47830/api/register -d '{"username":"ada","password":"a long passphrase"}'
 * curl -c jar -b jar -X POST localhost:47830/api/login -d '{"username":"ada","password":"a long passphrase"}'
 * curl -b jar localhost:47830/members            # 200: the members page, now that the cookie is a session
 * curl -b jar -X POST localhost:47830/api/logout # revokes the session row and clears the cookie
 * ```
 *
 * ## What this composes, and where each piece lives
 *
 * Nothing here is new machinery. This file is a demonstration that the engine's web surface is a set of
 * orthogonal primitives that a real deployment stacks:
 *
 *   - `web_server` shows the router, the generated OpenAPI document, the discoverability surface and a
 *     signed mirror attestation, with no user store.
 *   - `accounts_api` shows a real login: register, a password, a session that is a database row, and
 *     authorization that only ever shows a person their own rows.
 *   - `ui_ssr` shows one immediate-mode component served to a browser, with the social embedding metadata
 *     woven into its `<head>`.
 *
 * This one is a small content site that reaches for all of them at once, which is what a person actually
 * building a site does. The home page is server-rendered through the HTML presenter and carries a real
 * `NYA_PageMeta`, so a pasted link unfurls; the members page is the same rendering path behind the
 * accounts session; the discoverability documents and the metrics endpoint are merged routers; and gzip
 * is a single layer this file installs over the whole server. See ui_present_html.h, http_sitemap.h and
 * its siblings, debug_metrics.h, http_message.h and the accounts module.
 *
 * ## Why a compression layer and not a call per handler
 *
 * `nya_http_response_compress` is a pure function of the response and the request's Accept-Encoding, so a
 * handler could call it before it returns. Doing it once, as an outermost layer, is the honest shape: a
 * layer wraps every route the server has — the SSR pages, the JSON of `/oembed`, the Prometheus text, the
 * static discovery documents — so "compression is on" is a property of the server rather than a line each
 * handler has to remember. The layer runs the rest of the chain, then compresses whatever came back; the
 * function itself declines a body that is too small, already encoded, or of a type not worth compressing,
 * so the layer never has to decide any of that. See http_message.h.
 *
 * ## The rendering is SSR, not the live loop
 *
 * The pages are drawn once per request through the HTML presenter and sent. The event round trip that
 * makes ui_ssr *live* — a click posting back to `/event` and the changed HTML returning — is that
 * example's subject and is deliberately not here: a content site is read, not driven, so its pages are a
 * single draw pass with the client script left inert. The point being borrowed from ui_ssr is the
 * `<head>`: the same `NYA_PageMeta` value backs both the OpenGraph/Twitter tags and the `/oembed` answer.
 *
 * ## What is a limitation and what is on purpose
 *
 * The canonical URLs in the page metadata and the discoverability documents are written against a
 * canonical origin — `https://nyangine.example` — and not the loopback the example binds, because a
 * sitemap's `<loc>` and an `og:url` are absolute URLs a crawler and a chat app are meant to fetch, and
 * `127.0.0.1` is not one. A real deployment substitutes its own origin. Everything else — the login, the
 * session, the compression, the scrape — is exactly what a real server does.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"

/* STATE */

/** Default port. Loopback only; a deployment behind a reverse proxy sets its own and binds the wildcard. */
#define DEFAULT_PORT 47830

/**
 * The canonical origin the metadata and the discovery documents name.
 *
 * Not the loopback the server binds, on purpose: an `og:url` and a sitemap `<loc>` are absolute URLs a
 * crawler and a chat app fetch, so they point at where the site really lives. A real deployment sets this.
 * */
#define SITE_URL "https://nyangine.example"

/** How long the loop sleeps between ticks, and the frame cap the headless self test runs within. */
#define TICK_SLEEP_MS   4
#define SELF_TEST_FRAMES 2000

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

static void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

/** A windowless window: the UI keys its per-window tables on a NYA_Window, and never needs a real one here. */
NYA_INTERNAL NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 720,
    .screen_height = 480,
};

/** The HTML presenter the pages are drawn through, and its caller-owned buffer. */
NYA_INTERNAL NYA_UIHtml HTML;

/** The accounts database, under the save root. One connection outlives every request, so it is static. */
NYA_INTERNAL NYA_Arena*    DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database* DB       = nullptr;

/* THE CONTENT — one immediate-mode component per page, drawn for a browser */

/** The public home page: prose a crawler indexes and a link unfurls, drawn the way the GPU build would. */
NYA_INTERNAL void home_component(NYA_Window* window) {
    NYA_UI* ui = nya_ui_begin(window, NYA_UI_PASS_DRAW);

    if (nya_ui_panel_begin(ui, "home", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(560), .title = "nyangine · secure webapp" })) {
        nya_ui_label(ui, "A small content site, server-rendered in C.");
        nya_ui_label(ui, "This page is drawn once per request through the HTML presenter — the same");
        nya_ui_label(ui, "component code a desktop window or a terminal would run — and its <head> carries");
        nya_ui_label(ui, "OpenGraph and Twitter Card metadata, so a pasted link unfurls with a title and image.");
        nya_ui_label(ui, "");
        nya_ui_label(ui, "It is discoverable at /sitemap.xml, /feed.xml, /robots.txt and /llms.txt,");
        nya_ui_label(ui, "gzip-compressed on the wire, and scraped at /metrics in Prometheus format.");
        nya_ui_label(ui, "");
        nya_ui_label(ui, "The members area at /members is behind a real account login: register and");
        nya_ui_label(ui, "log in through /api, and the session cookie opens it.");

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** The gated members page, drawn once the account is known. Greets the person by the name they registered. */
NYA_INTERNAL void members_component(NYA_Window* window, NYA_ConstCString display) {
    NYA_UI* ui = nya_ui_begin(window, NYA_UI_PASS_DRAW);

    if (nya_ui_panel_begin(ui, "members", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(560), .title = "Members area" })) {
        char line[128] = { 0 };
        (void)snprintf(line, sizeof(line), "Signed in as %s.", display);
        nya_ui_label(ui, line);

        nya_ui_label(ui, "");
        nya_ui_label(ui, "This page is reached only with a valid session cookie: the same request without");
        nya_ui_label(ui, "one is answered 401, and robots.txt keeps it out of a crawler's index.");
        nya_ui_label(ui, "");
        nya_ui_label(ui, "POST /api/logout to end the session — the row is revoked, not only the cookie");
        nya_ui_label(ui, "cleared, so the token cannot be reused.");

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** Draws `component` into HTML and ends the frame, so this pass's presses do not linger into the next. */
NYA_INTERNAL void draw_once(void (*draw)(NYA_Window*, NYA_ConstCString), NYA_ConstCString arg) {
    nya_ui_html_reset(&HTML);
    draw(&WINDOW, arg);

    NYA_Event ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&ended);
}

/** home_component has no argument; this adapts it to draw_once's shape. */
NYA_INTERNAL void draw_home(NYA_Window* window, NYA_ConstCString unused) {
    nya_unused(unused);
    home_component(window);
}

/* THE PAGE METADATA — so a link to the home page unfurls, and /oembed answers from the same value */

/**
 * The social-media embedding metadata for the home page.
 *
 * One value backs both the `<head>` tags (nya_ui_html_document_meta) and the `/oembed` answer
 * (nya_ui_page_meta_oembed), which is the point of a single composable metadata value. The URLs are the
 * canonical origin's, not the loopback's, so a pasted link resolves to where the site really lives.
 * */
NYA_INTERNAL NYA_PageMeta home_meta(void) {
    return (NYA_PageMeta){
        .title         = "nyangine · secure webapp",
        .description   = "A small content site, server-rendered in C — social embedding, discoverability, gzip, metrics and a real login.",
        .canonical_url = SITE_URL "/",
        .image_url     = SITE_URL "/preview.png",
        .image_alt     = "The nyangine secure web app example",
        .site_name     = "nyangine",
        .author_name   = "nyangine",
        .type          = "website",
        .twitter_card  = NYA_TWITTER_CARD_SUMMARY_LARGE_IMAGE,
        .locale        = "en_US",
        .oembed_url    = SITE_URL "/oembed?url=https%3A%2F%2Fnyangine.example%2F",
    };
}

/* THE COMPRESSION LAYER — gzip over the whole server, in one place */

/**
 * Runs the rest of the chain, then compresses whatever came back when the client asked for it.
 *
 * Outermost, so it wraps every route the server has. The function itself decides whether it is worth it —
 * a body under NYA_HTTP_COMPRESS_MIN_BYTES, one already encoded, or a type not worth compressing is left
 * alone — so this layer never has to, and a response simply goes out uncompressed when compression would
 * not help. The negotiation reads only the Accept-Encoding header, so this stays on the wire boundary.
 * See http_message.h.
 * */
NYA_INTERNAL NYA_HttpStatus compress_layer(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    NYA_HttpStatus status = nya_http_chain_next(exchange, next);

    NYA_ConstCString accept_encoding = nya_http_request_header(exchange->request, "accept-encoding");
    (void)nya_http_response_compress(exchange->response, exchange->arena, accept_encoding);

    return status;
}

/* SESSION GUARD — the account this request's cookie names, reused from accounts_api */

/**
 * The account this request's cookie names, or false with the 401 already the caller's to return.
 *
 * Reads the session token out of the `__Host-session` cookie and validates it against the row, so a
 * revoked, expired or forged cookie is nobody. This is the whole of the gate the members page needs.
 * */
NYA_INTERNAL b8 request_account(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
    nya_memset(out_user, 0, sizeof(*out_user));

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) return false;

    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    if (cookie.size >= sizeof(token)) return false;

    nya_memcpy(token, cookie.text, cookie.size);

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_validate(exchange->arena, token, &session).ok) return false;

    return nya_account_find_by_id(exchange->arena, session.user_id, out_user).ok;
}

/** Reads `username` and `password` out of a JSON body, refusing anything that is not both strings. */
NYA_INTERNAL b8 request_credentials(NYA_HttpExchange* exchange, OUT NYA_ConstCString* out_username, OUT NYA_ConstCString* out_password) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return false;

    NYA_Value* username = nya_object_get(body, "username");
    NYA_Value* password = nya_object_get(body, "password");

    if (username == nullptr || username->type != NYA_TYPE_STRING) return false;
    if (password == nullptr || password->type != NYA_TYPE_STRING) return false;

    *out_username = username->as_string;
    *out_password = password->as_string;

    return true;
}

/** The peer's address as the server sees it, for the session's own record of where it was opened. */
NYA_INTERNAL NYA_ConstCString request_address(NYA_HttpExchange* exchange) {
    return exchange->address[0] != '\0' ? exchange->address : "unknown";
}

/* SSR HELPERS */

/**
 * Sets the per-response Content-Security-Policy for an SSR page: a fresh nonce for the one inline script
 * the presenter emits, written into `out_nonce`, and the tightest policy the presenter's own inline style
 * allows. `style-src 'unsafe-inline'` is the one loosening, safe because the presenter escapes every
 * label, so nothing a value carries becomes style of its own — the same bargain ui_ssr strikes.
 * */
NYA_INTERNAL b8 ssr_security_policy(NYA_HttpExchange* exchange, OUT char* out_nonce, u64 nonce_capacity) {
    u8 nonce_bytes[16] = { 0 };
    if (!nya_os_random_bytes(nonce_bytes, sizeof(nonce_bytes))) return false;

    u64 nonce_len = 0;
    if (!nya_crypto_base64url_encode(nonce_bytes, sizeof(nonce_bytes), out_nonce, nonce_capacity, &nonce_len)) return false;

    char policy[256] = { 0 };
    (void)snprintf(policy, sizeof(policy),
                   "default-src 'none'; img-src data:; style-src 'unsafe-inline'; script-src 'nonce-%s'; connect-src 'self'; base-uri 'none'; form-action 'none'",
                   out_nonce);

    return nya_http_response_header(exchange->response, "Content-Security-Policy", policy).ok;
}

/* HANDLERS: THE PAGES */

/** `GET /`: the home page, server-rendered, with the embedding metadata woven into its `<head>`. */
NYA_INTERNAL NYA_HttpStatus handle_home(NYA_HttpExchange* exchange) {
    draw_once(draw_home, nullptr);

    char nonce[32] = { 0 };
    if (!ssr_security_policy(exchange, nonce, sizeof(nonce))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_PageMeta meta = home_meta();

    static char page[NYA_UI_HTML_MAX + 8192];
    u32         written = nya_ui_html_document_meta(&HTML, page, sizeof(page), meta.title, nonce, &meta);
    if (written == 0) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)page, written, NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * `GET /members`: the gated page. Without a valid session it is a 401 that points at the login, and the
 * members content is never rendered; with one it is the members component, drawn for that account.
 * */
NYA_INTERNAL NYA_HttpStatus handle_members(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };

    if (!request_account(exchange, &user)) {
        // A single inert page rather than the component: nothing behind the gate is drawn for a request that has not passed it. 401, because it is the credentials — their absence — that were refused.
        NYA_ConstCString locked = "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
                                  "<title>Members — sign in</title>"
                                  "<body style=\"font-family:system-ui;max-width:40rem;margin:3rem auto;padding:0 1rem\">"
                                  "<h1>Members area</h1>"
                                  "<p>This page is for members. Register and log in through the API, then reload:</p>"
                                  "<pre>curl -c jar -X POST /api/register -d '{\"username\":\"ada\",\"password\":\"a long passphrase\"}'\n"
                                  "curl -c jar -b jar -X POST /api/login -d '{\"username\":\"ada\",\"password\":\"a long passphrase\"}'\n"
                                  "curl -b jar /members</pre>"
                                  "<p><a href=\"/\">Back to the home page</a></p></body></html>";

        NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)locked, (u64)strlen(locked), NYA_HTTP_MEDIA_HTML);

        // The bytes going out is not the point; the status is, so a failure to write still answers 401.
        nya_unused(sent);
        return NYA_HTTP_STATUS_UNAUTHORIZED;
    }

    NYA_ConstCString display = user.display[0] != '\0' ? user.display : user.username;
    draw_once(members_component, display);

    char nonce[32] = { 0 };
    if (!ssr_security_policy(exchange, nonce, sizeof(nonce))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // No NYA_PageMeta here: a members page is private, robots.txt keeps it out of an index, and there is nothing to unfurl. nya_ui_html_document is nya_ui_html_document_meta without the head tags.
    static char page[NYA_UI_HTML_MAX + 8192];
    u32         written = nya_ui_html_document(&HTML, page, sizeof(page), "Members area", nonce);
    if (written == 0) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_bytes(exchange->response, (const u8*)page, written, NYA_HTTP_MEDIA_HTML);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * `GET /oembed?url=…`: the structured metadata a consumer that found the discovery `<link>` fetches, as an
 * oEmbed 1.0 JSON document built by the engine from the same NYA_PageMeta the head carries. The `url`
 * parameter is required and must be an http(s) URL — the same gate the head's URLs pass.
 * */
NYA_INTERNAL NYA_HttpStatus handle_oembed(NYA_HttpExchange* exchange) {
    char url[512] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "url", url, sizeof(url))) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!nya_ui_page_meta_url_ok(url)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_PageMeta meta     = home_meta();
    NYA_Object*  document = nullptr;
    if (!nya_ui_page_meta_oembed(exchange->arena, &meta, &document).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error sent = nya_http_response_json(exchange->response, exchange->arena, document);

    return sent.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* HANDLERS: THE LOGIN, reused from accounts_api */

/** Registration, open to anybody in this example. A taken name or a short password says which. */
NYA_INTERNAL NYA_HttpStatus handle_register(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user = { 0 };
    NYA_Error       made = nya_account_register(exchange->arena, NYA_ACCOUNT_REGISTRATION_OPEN, username, password, nullptr, &user);

    if (!made.ok) {
        // A taken name and a bad password are the caller's to fix; anything else is a server fault.
        if (made.kind == NYA_ERROR_ALREADY_EXISTS || made.kind == NYA_ERROR_INVALID_ARGUMENT) return NYA_HTTP_STATUS_UNPROCESSABLE;

        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/**
 * A password, and on success a session in the `__Host-session` cookie. One refusal covers every way the
 * password step can fail, so nothing here tells a guesser which usernames exist, and the throttle in
 * `accounts` slows a guessing spree whatever the outcome. There is no second factor in this example; the
 * two-step login is accounts_api's subject.
 * */
NYA_INTERNAL NYA_HttpStatus handle_login(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user    = { 0 };
    NYA_Error       allowed = nya_account_authenticate(exchange->arena, username, password, request_address(exchange), &user);

    // One answer for every way it fails — wrong password, no such user, disabled, throttled.
    if (!allowed.ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_ConstCString agent = nya_http_request_header(exchange->request, "user-agent");

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_issue(exchange->arena, user.id, request_address(exchange), agent, &session).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = NYA_HTTP_SESSION_COOKIE,
                                                 .value     = session.token,
                                                 .max_age_s = NYA_ACCOUNTS_SESSION_IDLE_S,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                             });

    return set.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Signing out: the session row is revoked, not only the cookie cleared, so the token cannot be reused. */
NYA_INTERNAL NYA_HttpStatus handle_logout(NYA_HttpExchange* exchange) {
    NYA_HttpCookieValue cookie = { 0 };

    if (nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) {
        char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };

        if (cookie.size < sizeof(token)) {
            nya_memcpy(token, cookie.text, cookie.size);

            NYA_AccountSession session = { 0 };
            if (nya_account_session_validate(exchange->arena, token, &session).ok) {
                (void)nya_account_session_revoke(exchange->arena, session.id);
            }
        }
    }

    NYA_Error cleared = nya_http_response_cookie_clear(exchange->response, NYA_HTTP_SESSION_COOKIE, "/", true);

    return cleared.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* ROUTES */

/*
 * Every route is NYA_HTTP_AFFINITY_MAIN: the SSR pages share the one HTML presenter and window, and the
 * account handlers touch the one database, both on the ticking thread and neither shareable across
 * workers. Two requests never race the presenter or the same rows because they are answered in turn from
 * the loop below.
 */
NYA_INTERNAL const NYA_HttpRoute PAGE_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = "/", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_home,
      .summary = "The home page, server-rendered with embedding metadata",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_GET, .path = "/members", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_members,
      .summary = "The members page, behind the account session",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method = NYA_HTTP_METHOD_GET, .path = "/oembed", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_oembed,
      .summary = "oEmbed metadata for a link to the home page",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

NYA_INTERNAL const NYA_HttpRouter PAGE_ROUTER = {
    .name = "pages", .routes = PAGE_ROUTES, .route_count = nya_carray_length(PAGE_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute ACCOUNT_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = "/api/register", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_register,
      .summary = "Makes an account",
      .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/api/login", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login,
      .summary = "Logs in and sets the session cookie",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/api/logout", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_logout,
      .summary = "Revokes the session and clears the cookie",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

NYA_INTERNAL const NYA_HttpRouter ACCOUNT_ROUTER = {
    .name = "accounts", .routes = ACCOUNT_ROUTES, .route_count = nya_carray_length(ACCOUNT_ROUTES),
};

/* THE DISCOVERABILITY SURFACE */

/**
 * Builds and mounts the four well-known documents a crawler and an LLM look for. Merge
 * nya_http_doc_router() after to bring them online; nya_http_doc_clear() frees them on the way out.
 * The URLs are the canonical origin's, for the reason home_meta's are. See http_sitemap.h and siblings.
 * */
NYA_INTERNAL NYA_Error mount_discovery(void) {
    // The sitemap: the pages worth crawling. /members is not here — it is private — and neither is the API.
    const NYA_HttpSitemapUrl sitemap_urls[] = {
        { .loc = SITE_URL "/", .changefreq = NYA_HTTP_SITEMAP_DAILY, .priority = 1.0F, .has_priority = true },
    };
    NYA_TRY(nya_http_sitemap_mount((NYA_HttpSitemapConfig){ .urls = sitemap_urls, .count = nya_carray_length(sitemap_urls) }));

    // The feed, RSS at /feed.xml and Atom at /atom.xml from one set of items.
    const NYA_HttpFeedItem feed_items[] = {
        { .title         = "The secure web app example is live",
          .link          = SITE_URL "/",
          .description   = "Social embedding, discoverability, gzip, metrics & a real login, composed.",
          .published     = nya_instant_now(),
          .has_published = true },
    };
    NYA_TRY(nya_http_feed_mount((NYA_HttpFeedConfig){
        .title       = "nyangine secure webapp",
        .link        = SITE_URL "/",
        .description = "The example content site's feed",
        .self_link   = SITE_URL NYA_HTTP_FEED_PATH,
        .updated     = nya_instant_now(),
        .has_updated = true,
        .items       = feed_items,
        .count       = nya_carray_length(feed_items),
    }));

    // robots.txt: keep the members area, the API and the scrape endpoint out of a crawler's index, and point it at the sitemap.
    const NYA_ConstCString    robots_disallow[] = { "/members", "/api/", "/metrics" };
    const NYA_HttpRobotsGroup robots_groups[]   = {
        { .user_agent = "*", .disallow = robots_disallow, .disallow_count = nya_carray_length(robots_disallow) },
    };
    NYA_TRY(nya_http_robots_mount((NYA_HttpRobotsConfig){
        .groups = robots_groups, .count = nya_carray_length(robots_groups), .sitemap = SITE_URL NYA_HTTP_SITEMAP_PATH }));

    // llms.txt, the strict preset: the document states its content is not for training, then points a model at the home page it may read.
    const NYA_HttpLlmsLink llms_links[] = {
        { .title = "Home", .url = SITE_URL "/", .note = "the content site itself" },
    };
    const NYA_HttpLlmsSection llms_sections[] = { { .heading = "Pages", .links = llms_links, .link_count = nya_carray_length(llms_links) } };

    return nya_http_llms_mount_strict((NYA_HttpLlmsConfig){
        .name = "nyangine secure webapp", .summary = "An example content site built on nyangine.", .sections = llms_sections, .section_count = nya_carray_length(llms_sections) });
}

/* THE HEADLESS SELF TEST — the server proves itself, on a thread of its own */

/** Whether `needle` occurs in the `size` bytes at `hay`. A raw body is bytes, not a C string, so no strstr. */
NYA_INTERNAL b8 bytes_contain(const u8* hay, u64 size, NYA_ConstCString needle) {
    u64 needle_len = strlen(needle);
    if (needle_len == 0 || size < needle_len) return false;

    for (u64 i = 0; i + needle_len <= size; i++) {
        if (nya_memcmp(hay + i, needle, needle_len) == 0) return true;
    }

    return false;
}

/** What the self test shares with the main loop: where to reach the server, and how it turned out. */
typedef struct {
    u16                     port;
    volatile sig_atomic_t*  running;
    volatile sig_atomic_t   ok;
} SelfTest;

/**
 * Fetches a few of the server's own endpoints through the HTTP client and checks what came back, then
 * clears `running` so the loop that answers these requests can stop. It is a thread of its own because
 * the routes are answered on the main loop: a blocking request from that same thread would wait for a
 * tick that is waiting for it. Here the loop keeps ticking while this thread blocks on each fetch.
 * */
NYA_INTERNAL void self_test_run(void* data) {
    SelfTest* test = (SelfTest*)data;

    NYA_Arena* arena = nya_arena_create(.name = "self_test");
    defer      nya_arena_destroy(arena);

    char base[64] = { 0 };
    (void)snprintf(base, sizeof(base), "http://127.0.0.1:%u", test->port);

    b8 ok = true;

    // 1. GET / — a 200, and the OpenGraph tags in the head, so a link to it unfurls.
    {
        char url[80] = { 0 };
        (void)snprintf(url, sizeof(url), "%s/", base);

        NYA_Response response = { 0 };
        NYA_Error    got      = nya_request_get(arena, url, &response);

        b8 has_og = response.raw_body != nullptr && bytes_contain(response.raw_body->items, response.raw_body->length, "og:title")
                    && bytes_contain(response.raw_body->items, response.raw_body->length, "twitter:card");

        nya_log_info("GET /            -> %u, %llu bytes, og:/twitter: tags %s", response.status,
                     response.raw_body != nullptr ? (unsigned long long)response.raw_body->length : 0ULL, has_og ? "present" : "MISSING");

        ok = ok && got.ok && response.status == 200 && has_og;
    }

    // 2. GET /metrics — the Prometheus text exposition, which a scrape reads.
    {
        char url[80] = { 0 };
        (void)snprintf(url, sizeof(url), "%s/metrics", base);

        NYA_Response response = { 0 };
        NYA_Error    got      = nya_request_get(arena, url, &response);

        b8 is_prometheus = response.raw_body != nullptr && bytes_contain(response.raw_body->items, response.raw_body->length, "# HELP")
                           && bytes_contain(response.raw_body->items, response.raw_body->length, "# TYPE");

        nya_log_info("GET /metrics     -> %u, Prometheus text %s", response.status, is_prometheus ? "present" : "MISSING");

        ok = ok && got.ok && response.status == 200 && is_prometheus;
    }

    // 3. GET / with Accept-Encoding: gzip — a compressed response, Content-Encoding naming the coding.
    {
        char url[80] = { 0 };
        (void)snprintf(url, sizeof(url), "%s/", base);

        NYA_Response response = { 0 };
        NYA_Error    got      = nya_request_perform(arena,
                                                 (NYA_Request){
                                                        .method  = NYA_REQUEST_METHOD_GET,
                                                        .url     = url,
                                                        .headers = { { .name = "Accept-Encoding", .value = "gzip" } },
                                                 },
                                                 &response);

        char encoding[32] = { 0 };
        b8   gzipped      = nya_response_header(&response, "Content-Encoding", encoding, sizeof(encoding)) && strstr(encoding, "gzip") != nullptr;

        nya_log_info("GET / (gzip)     -> %u, Content-Encoding: %s, %llu compressed bytes", response.status, gzipped ? encoding : "(none)",
                     response.raw_body != nullptr ? (unsigned long long)response.raw_body->length : 0ULL);

        ok = ok && got.ok && response.status == 200 && gzipped;
    }

    // 4. GET /members — the gate, unauthenticated: a 401 and no members content.
    {
        char url[80] = { 0 };
        (void)snprintf(url, sizeof(url), "%s/members", base);

        NYA_Response response = { 0 };
        NYA_Error    got      = nya_request_get(arena, url, &response);
        nya_unused(got);

        nya_log_info("GET /members     -> %u (gated; a session cookie opens it)", response.status);

        ok = ok && response.status == 401;
    }

    test->ok = ok ? 1 : 0;
    nya_log_info("Self test: %s", ok ? "PASS" : "FAIL");

    // Let the loop that has been answering these stop now that they are done.
    *test->running = 0;
}

/* MAIN */

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    // Default is the headless self test, so `./build run example secure_webapp` proves itself and exits. --serve is the persistent server, for a browser.
    b8 serve = false;

    for (s32 i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serve") == 0) serve = true;

        if (i + 1 >= argc) continue;

        if (strcmp(argv[i], "--port") == 0) {
            if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
                nya_log_error("--port expects a number from 0 to 65535, got '%s'.", argv[i + 1]);
                return EXIT_FAILURE;
            }
        }
    }

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();

    /* No window, no renderer, no frame loop. What comes up is the set the SSR path reads through — an app instance, the settings, callback and event registries, the input system a focused field would reach for, the window system that lookup resolves against, and the asset system a style may use — plus the save root and the accounts database. The same set ui_ssr and accounts_api bring up between them. */
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_settings_init();

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init(), "while starting the event registry");
    defer nya_system_events_deinit();

    nya_system_input_init();
    defer nya_system_input_deinit();

    nya_system_window_init();
    defer nya_system_window_deinit();

    nya_system_asset_init();
    defer nya_system_asset_deinit();

    NYA_Error saves = nya_system_save_init();
    if (!saves.ok) {
        nya_log_error("No save root, so there is nowhere to keep the accounts: %s", (NYA_ConstCString)saves.message);
        return EXIT_FAILURE;
    }
    defer nya_system_save_deinit();

    DB_ARENA = nya_arena_create(.name = "accounts_db");
    defer    nya_arena_destroy(DB_ARENA);

    NYA_Error stored = nya_save_database_open(DB_ARENA, "secure_webapp.db", &DB);
    if (!stored.ok) {
        nya_log_error("Could not open the accounts database: %s", (NYA_ConstCString)stored.message);
        return EXIT_FAILURE;
    }
    defer nya_sql_close(DB);

    // The accounts module and its tables, on this database.
    NYA_EXPECT(nya_accounts_open(DB_ARENA, DB), "while opening the accounts tables");
    defer nya_accounts_close();

    nya_ui_html_init(&HTML, NYA_UI_HTML_CELL);
    nya_ui_presenter_set(&WINDOW, nya_ui_html_presenter(&HTML));
    defer nya_ui_html_deinit(&HTML);

    // One record per request at the headers level, and the compression layer outermost so it wraps every route. Single-threaded: every route is MAIN, so the loop below is the only thing answering, and the self test's requests come in on a thread of their own.
    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    static const NYA_HttpLayerFn LAYERS[] = { compress_layer, nya_http_layer_log };

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
                   .port        = port,
                   .workers     = 0,
                   .layers      = LAYERS,
                   .layer_count = nya_carray_length(LAYERS),
               }),
               "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&PAGE_ROUTER), "while mounting the pages");
    defer nya_http_server_unmerge(&PAGE_ROUTER);

    NYA_EXPECT(nya_http_server_merge(&ACCOUNT_ROUTER), "while mounting the accounts routes");
    defer nya_http_server_unmerge(&ACCOUNT_ROUTER);

    // GET /metrics, the Prometheus scrape, and its /api/metrics siblings. Merging is the opt-in.
    NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()), "while mounting the metrics resource");
    defer nya_http_server_unmerge(nya_http_metrics_router());

    // /openapi.json and /docs, generated by walking every merged route table above.
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while mounting the generated document");
    defer nya_http_server_unmerge(nya_http_openapi_router());

    // The discoverability documents: /sitemap.xml, /feed.xml and /atom.xml, /robots.txt and /llms.txt.
    NYA_EXPECT(mount_discovery(), "while building the discoverability documents");
    defer nya_http_doc_clear();

    NYA_EXPECT(nya_http_server_merge(nya_http_doc_router()), "while mounting the discoverability documents");
    defer nya_http_server_unmerge(nya_http_doc_router());

    nya_log_info("secure_webapp on http://127.0.0.1:%u", nya_http_server_port());
    nya_log_info("  /            the home page, SSR with OpenGraph/Twitter meta and an oEmbed discovery link");
    nya_log_info("  /members     the members page, behind the account session");
    nya_log_info("  /oembed      the oEmbed metadata the home page's head points at");
    nya_log_info("  /metrics     the Prometheus text exposition, for a scrape");
    nya_log_info("  discoverable at " NYA_HTTP_SITEMAP_PATH ", " NYA_HTTP_FEED_PATH ", " NYA_HTTP_FEED_ATOM_PATH ", " NYA_HTTP_ROBOTS_PATH " and " NYA_HTTP_LLMS_PATH);
    nya_log_info("  gzip on the wire for every route that is worth compressing");

    /* --serve: the persistent server, answered from this loop until ctrl-c. This is the shape a real deployment runs. */
    if (serve) {
        nya_log_info("Serving until interrupted (ctrl-c). Register, log in, and open /members.");

        while (RUNNING) {
            nya_system_http_tick();
            nya_os_time_sleep_ms(TICK_SLEEP_MS);
        }

        nya_log_info("Stopping after " FMTu64 " requests.", nya_http_server_request_count());
        return EXIT_SUCCESS;
    }

    /* The default: the headless self test. The checks run on a thread of their own — they block on the HTTP client, and the routes they hit are answered from this loop, so the two cannot be the same thread. The loop ticks until the test clears RUNNING, or until the frame cap, whichever comes first. */
    nya_log_info("Headless self test (pass --serve to keep serving instead):");

    SelfTest test = { .port = nya_http_server_port(), .running = &RUNNING, .ok = 0 };

    NYA_Arena*  test_arena  = nya_arena_create(.name = "self_test_thread");
    defer       nya_arena_destroy(test_arena);
    NYA_Thread* test_thread = nullptr;

    NYA_EXPECT(nya_thread_spawn(test_arena, self_test_run, &test, "self_test", &test_thread), "while starting the self test");

    for (u32 frame = 0; RUNNING && frame < SELF_TEST_FRAMES; frame++) {
        nya_system_http_tick();
        nya_os_time_sleep_ms(TICK_SLEEP_MS);
    }

    // Wait for the test to finish however it went — a pass cleared RUNNING, a hang hit the frame cap — so nothing of its is running before the server and its arenas come down through the defers above.
    nya_thread_join(test_thread);

    nya_log_info("Stopping after " FMTu64 " requests.", nya_http_server_request_count());

    return test.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
