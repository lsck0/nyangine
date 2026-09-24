/**
 * The login flow end to end: a real port, a real socket, and a session that lives in a cookie.
 *
 * test_accounts.c proves the accounts module in the small — a password nobody can reverse, a login
 * that says the same thing however it fails, a session that a token opens. This proves the other
 * half: those calls behind three HTTP routes, the way the accounts_api example wires them, driven over
 * a loopback socket the way a browser drives them. What is asserted here is the seam between them —
 * that logging in hands back a `__Host-session` cookie with the flags a session cookie must carry, that
 * the cookie alone is enough to be recognised on the next request, that a wrong password is refused
 * with no cookie at all, and that logging out revokes the row so the very same cookie is nobody after.
 *
 * The server is driven by hand rather than by a frame loop: a request is written, nya_system_http_tick
 * runs the handler on this thread (every route is NYA_HTTP_AFFINITY_MAIN, so the one in-memory database
 * is only ever touched here), and the answer is read back. The database is `:memory:`, so the whole
 * test touches no file and races nothing.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <string.h>
#include <time.h>

/** Long enough for the module to accept it, and nothing anybody would use. */
#define PASSWORD "a correct horse battery staple"

/* ROUTES: the smallest honest login, the accounts_api example's three handlers with nothing else on them */

/** The account this request's cookie names, or false with the 401 the caller returns. Mirrors accounts_api. */
static b8 request_account(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) return false;

    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    if (cookie.size >= sizeof(token)) return false;

    nya_memcpy(token, cookie.text, cookie.size);

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_validate(exchange->arena, token, &session).ok) return false;

    return nya_account_find_by_id(exchange->arena, session.user_id, out_user).ok;
}

/** A password, and on success an opaque session token in a `__Host-session` cookie. 401 for every failure. */
static NYA_HttpStatus handle_login(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* username = nya_object_get(body, "username");
    NYA_Value* password = nya_object_get(body, "password");

    if (username == nullptr || username->type != NYA_TYPE_STRING) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (password == nullptr || password->type != NYA_TYPE_STRING) return NYA_HTTP_STATUS_BAD_REQUEST;

    // One refusal for every way the credentials can be wrong, at the same cost whether the account
    // exists or not: it is nya_account_authenticate that guarantees that, and this route says nothing
    // more than it does.
    NYA_AccountUser user    = { 0 };
    NYA_Error       allowed = nya_account_authenticate(exchange->arena, username->as_string, password->as_string, exchange->address, &user);
    if (!allowed.ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_issue(exchange->arena, user.id, exchange->address, "test-agent", &session).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

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

/** Who the cookie says you are, as JSON. 401 when it says nobody. */
static NYA_HttpStatus handle_me(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = user.id });
    nya_object_add(body, "username", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user.username });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Signing out revokes the row, not only clears the cookie, so the token cannot be replayed. */
static NYA_HttpStatus handle_logout(NYA_HttpExchange* exchange) {
    NYA_HttpCookieValue cookie = { 0 };

    if (nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) {
        char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };

        if (cookie.size < sizeof(token)) {
            nya_memcpy(token, cookie.text, cookie.size);

            NYA_AccountSession session = { 0 };
            if (nya_account_session_validate(exchange->arena, token, &session).ok) (void)nya_account_session_revoke(exchange->arena, session.id);
        }
    }

    NYA_Error cleared = nya_http_response_cookie_clear(exchange->response, NYA_HTTP_SESSION_COOKIE, "/", true);

    return cleared.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static const NYA_HttpRoute LOGIN_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = "/api/login", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login,
      .summary = "Logs in; sets the session cookie",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = "/api/logout", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_logout,
      .summary = "Revokes the session and clears the cookie", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_GET, .path = "/api/me", .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_me,
      .summary = "Who the cookie says you are", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
};

static const NYA_HttpRouter LOGIN_ROUTER = {
    .name = "login", .routes = LOGIN_ROUTES, .route_count = nya_carray_length(LOGIN_ROUTES),
};

/* THE LOOPBACK HARNESS: a real port, a real socket, one whole answer at a time */

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

/** Connects to the server, waiting for the non-blocking connect to come up. */
static NYA_OsSocket connect_to(u16 port) {
    NYA_OsAddress address = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

    NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);
    nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

    NYA_OsSocketWait watched = { .socket = socket, .writable = true };
    u32              ready   = 0;

    nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
    nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

    return socket;
}

/**
 * Sends `text`, ticks the server, and reads back a whole answer: the head, then exactly the body its
 * Content-Length promises. Every answer here carries one — a no-content reply renders `Content-Length: 0`
 * — so the length is always what says the answer is complete.
 * */
static u64 exchange(NYA_OsSocket socket, NYA_ConstCString text, OUT char* buffer, u64 capacity) {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(socket, (const u8*)text, strlen(text), &wrote) == NYA_OS_SOCKET_OK && wrote == strlen(text));

    u64 filled   = 0;
    u64 expected = 0;

    for (u32 attempt = 0; attempt < 400 && filled + 1 < capacity; attempt++) {
        nya_system_http_tick();

        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)(buffer + filled), capacity - filled - 1, &read);
        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) break;

        filled         += read;
        buffer[filled]  = '\0';

        if (expected == 0) {
            const char* blank  = strstr(buffer, "\r\n\r\n");
            const char* length = strstr(buffer, "Content-Length: ");
            if (blank != nullptr && length != nullptr) expected = (u64)(blank + 4 - buffer) + strtoull(length + 16, nullptr, 10);
        }

        if (expected > 0 && filled >= expected) break;

        sleep_ms(2);
    }

    buffer[filled] = '\0';
    return filled;
}

/** The status code out of a response's status line, or zero when it is not one this recognises. */
static u32 status_of(const char* answer) {
    if (strncmp(answer, "HTTP/1.1 ", 9) != 0) return 0;
    return (u32)strtoul(answer + 9, nullptr, 10);
}

/**
 * Copies the `__Host-session` token out of the response's Set-Cookie line, or false when there is none.
 *
 * The value runs from after the `=` to the first `;`, which is where the attributes begin.
 * */
static b8 session_cookie_token(const char* answer, OUT char* out_token, u64 capacity) {
    out_token[0] = '\0';

    const char* set = strstr(answer, "Set-Cookie: " NYA_HTTP_SESSION_COOKIE "=");
    if (set == nullptr) return false;

    const char* value = set + strlen("Set-Cookie: " NYA_HTTP_SESSION_COOKIE "=");
    const char* end   = strchr(value, ';');
    if (end == nullptr) return false;

    u64 length = (u64)(end - value);
    if (length == 0 || length + 1 > capacity) return false;

    nya_memcpy(out_token, value, length);
    out_token[length] = '\0';
    return true;
}

/* MAIN */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_login_flow");
    defer      nya_arena_destroy(arena);

    // A clean throttle and a clean in-memory database: the flow starts from nothing.
    nya_account_throttle_reset();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    NYA_EXPECT(nya_accounts_open(arena, db));
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser created = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &created));

    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port }), "while starting the login test server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&LOGIN_ROUTER), "while mounting the login routes");

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };
    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };

    // TEST: a right password logs in and hands back a session cookie with every flag a session needs
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        const char* body    = "{\"username\":\"ada\",\"password\":\"" PASSWORD "\"}";
        NYA_String* request = nya_string_sprintf(arena,
                                                 "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                                 "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
                                                 strlen(body), body);

        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_check(status_of(answer) == 204, "a right password logs in, got '%.15s'", answer);
        nya_check(session_cookie_token(answer, token, sizeof(token)), "and the answer carries a __Host-session cookie");
        nya_check(token[0] != '\0', "with a token in it");

        // The flags that make it a session cookie: unreadable from script, sent over TLS only, and not
        // sent on a cross-site request — the three that keep the token out of an injected script's reach
        // and off a forged cross-site POST. Path=/ and the __Host- prefix pin it to this exact origin.
        nya_check(nya_string_contains(answer, "HttpOnly"), "the cookie is HttpOnly");
        nya_check(nya_string_contains(answer, "Secure"), "the cookie is Secure");
        nya_check(nya_string_contains(answer, "SameSite=Strict"), "the cookie is SameSite=Strict");
        nya_check(nya_string_contains(answer, "Path=/"), "the cookie is scoped to the whole origin");
    }

    // TEST: the cookie alone is who you are on the next request
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = nya_string_sprintf(arena,
                                                 "GET /api/me HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE
                                                 "=%s\r\nConnection: keep-alive\r\n\r\n",
                                                 token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_check(status_of(answer) == 200, "the session cookie is recognised, got '%.15s'", answer);
        nya_check(nya_string_contains(answer, "\"username\":\"ada\""), "and names the account it belongs to");
    }

    // TEST: no cookie is nobody
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        nya_assert(exchange(client, "GET /api/me HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n", answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 401, "a request with no cookie is nobody, got '%.15s'", answer);
    }

    // TEST: logging out revokes the row, so the very same cookie is nobody afterwards
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* logout = nya_string_sprintf(arena,
                                                "POST /api/logout HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE
                                                "=%s\r\nConnection: keep-alive\r\n\r\n",
                                                token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, logout), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 204, "logging out succeeds, got '%.15s'", answer);

        // The same token, now against a revoked row: a signed claim would still be valid here, and this
        // is the whole reason a session is a row instead of one.
        NYA_String* after = nya_string_sprintf(arena,
                                               "GET /api/me HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE
                                               "=%s\r\nConnection: keep-alive\r\n\r\n",
                                               token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, after), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 401, "and the revoked cookie is nobody, got '%.15s'", answer);
    }

    // TEST: a wrong password is refused, with no cookie handed out to guess with
    {
        nya_account_throttle_reset();

        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        const char* body    = "{\"username\":\"ada\",\"password\":\"the wrong password entirely\"}";
        NYA_String* request = nya_string_sprintf(arena,
                                                 "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                                 "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
                                                 strlen(body), body);

        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_check(status_of(answer) == 401, "a wrong password is refused, got '%.15s'", answer);
        nya_check(!nya_string_contains(answer, "Set-Cookie"), "and nothing is set that could be a session");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
