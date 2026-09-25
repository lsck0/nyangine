/**
 * The http_server, held to what a pentest pass over it asked of it: authn/authz bypass, session
 * fixation, CSRF, injection through the ORM, and header (response) splitting. Every finding a scan
 * over the accounts_api example turns up is written here as an assertion, so a regression that reopens
 * one fails a test rather than shipping.
 *
 * Driven in-process through nya_http_router_dispatch, the way tests/nyangine/http/test_router.c does:
 * a route table and an exchange are data, and exercising the server's own defences needs no port and
 * no socket. The database is `:memory:`, so the whole file touches no file and races nothing, and there
 * is not a sleep in it. The handlers under test are the accounts_api example's own — the caller read
 * from a `__Host-session` cookie, an owner-filtered ORM query, and an owner check that answers "not
 * found" rather than "forbidden" — mounted on the smallest routes that reach them.
 *
 * What each block locks in is stated where it runs. Where the engine already defends, the assertion
 * keeps it defended; nothing here found a live bypass to fix.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <string.h>

/** Long enough for the accounts module to accept it, and nothing anybody would use. */
#define PASSWORD    "a correct horse battery staple"
#define REPLACEMENT "a different horse entirely, also long"

/** Who is asking, which is what the session list and the throttle keep. */
#define ADDRESS "203.0.113.9"
#define AGENT   "test-agent"

/** A fixed "now", so nothing here depends on the wall clock. */
#define NOW_S 1700000000ULL

/** Long enough to be accepted, and obviously not a real secret. The bearer routes verify against it. */
static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

/* THE NOTES TABLE: the accounts_api example's Model, written out by hand the way its own header does */

/** Longest stored note, terminator included. */
#define NOTE_TEXT_MAX 280

/** One note and who owns it. `owner` is the account id; nothing is ever read across owners. */
typedef struct {
    s64  id;
    s64  owner;
    s64  written_at_s;
    char text[NOTE_TEXT_MAX];
} SecNote;

static const NYA_TypeReflection SEC_NOTE_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NOTE_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NOTE_TEXT_MAX,
};

static const NYA_ReflectField SEC_NOTE_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(SecNote, id), .is_key = true },
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(SecNote, owner) },
    { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(SecNote, written_at_s) },
    { .name = "text", .type = &SEC_NOTE_TEXT_ARRAY, .offset = nya_offsetof(SecNote, text) },
};

static const NYA_TypeReflection SEC_NOTE_MODEL = {
    .name        = "SecNote",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(SecNote),
    .alignment   = alignof(SecNote),
    .fields      = SEC_NOTE_FIELDS,
    .field_count = nya_carray_length(SEC_NOTE_FIELDS),
};

/** The one table every handler here touches. Set once in main. */
static NYA_OrmTable* NOTES = nullptr;

/* THE HANDLERS: the accounts_api example's own owner checks, on the smallest routes that reach them */

/**
 * The account this request's `__Host-session` cookie names, or false. This is the accounts_api example's
 * nya_http_accounts_caller in miniature: an opaque token, validated against the row, never a claim.
 * */
static b8 caller_of(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
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

/** The `id` query parameter as a number, or false when it is absent or not one. */
static b8 note_id_of(const NYA_HttpRequest* request, OUT u64* out_id) {
    char buffer[24] = { 0 };
    if (!nya_http_request_query_param(request, "id", buffer, sizeof(buffer))) return false;

    return nya_type_parse(NYA_TYPE_U64, (const u8*)buffer, strlen(buffer), out_id);
}

/** One note by id — the caller's own, or 404. Not yours is the same answer as not there: the IDOR read. */
static NYA_HttpStatus note_get(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!caller_of(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    u64 id = 0;
    if (!note_id_of(exchange->request, &id)) return NYA_HTTP_STATUS_BAD_REQUEST;

    SecNote note = { 0 };
    if (!nya_orm_find(NOTES, exchange->arena, nya_sql_s64((s64)id), &note).ok) return NYA_HTTP_STATUS_NOT_FOUND;

    // the whole point: somebody else's note is not found, so a caller cannot tell it apart from one that never existed.
    if (note.owner != (s64)user.id) return NYA_HTTP_STATUS_NOT_FOUND;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "id", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = note.id });
    nya_object_add(body, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)note.text });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Deletes one of the caller's notes by id — and only one of the caller's. The IDOR write. */
static NYA_HttpStatus note_delete(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!caller_of(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    u64 id = 0;
    if (!note_id_of(exchange->request, &id)) return NYA_HTTP_STATUS_BAD_REQUEST;

    SecNote note = { 0 };
    if (!nya_orm_find(NOTES, exchange->arena, nya_sql_s64((s64)id), &note).ok) return NYA_HTTP_STATUS_NOT_FOUND;
    if (note.owner != (s64)user.id) return NYA_HTTP_STATUS_NOT_FOUND;

    if (!nya_orm_delete(NOTES, nya_sql_s64((s64)id)).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/** How many notes the caller owns. The filter is on the owner, in the bound query, so it is only ever theirs. */
static NYA_HttpStatus notes_query(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!caller_of(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    void* rows  = nullptr;
    u32   count = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ? ORDER BY id", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &rows, &count).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = count });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Reflects an attacker-controlled query value straight into a response header — the classic
 * response-splitting setup. The writer refuses a CR or LF, so a value that decoded one is rejected here
 * rather than opening a second header.
 * */
static NYA_HttpStatus reflect_next(NYA_HttpExchange* exchange) {
    char next[256] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "next", next, sizeof(next))) return NYA_HTTP_STATUS_BAD_REQUEST;

    if (!nya_http_response_header(exchange->response, "X-Next", next).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    return nya_http_response_text(exchange->response, "ok", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** A read behind the bearer extractor, so an absent, tampered or expired token never reaches it. */
static NYA_HttpStatus secret_read(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity) {
    nya_unused(identity);

    return nya_http_response_text(exchange->response, "top secret", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* THE ROUTES */

static const NYA_HttpRoute NOTE_ROUTES[] = {
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = "/api/note",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = note_get,
     .summary  = "One of your notes by id",
     .statuses = { NYA_HTTP_STATUS_OK,
                    NYA_HTTP_STATUS_BAD_REQUEST,
                    NYA_HTTP_STATUS_UNAUTHORIZED,
                    NYA_HTTP_STATUS_NOT_FOUND,
                    NYA_HTTP_STATUS_INTERNAL_ERROR }                                                  },
    { .method   = NYA_HTTP_METHOD_DELETE,
     .path     = "/api/note",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = note_delete,
     .summary  = "Deletes a note you own",
     .statuses = { NYA_HTTP_STATUS_NO_CONTENT,
                    NYA_HTTP_STATUS_BAD_REQUEST,
                    NYA_HTTP_STATUS_UNAUTHORIZED,
                    NYA_HTTP_STATUS_NOT_FOUND,
                    NYA_HTTP_STATUS_FORBIDDEN,
                    NYA_HTTP_STATUS_INTERNAL_ERROR }                                                  },
    { .method   = NYA_HTTP_METHOD_QUERY,
     .path     = "/api/notes",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = notes_query,
     .summary  = "How many notes you own",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = "/api/echo",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = reflect_next,
     .summary  = "Reflects ?next into a header",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR }  },
};

static const NYA_HttpRouter NOTE_ROUTER = {
    .name        = "notes",
    .routes      = NOTE_ROUTES,
    .route_count = nya_carray_length(NOTE_ROUTES),
};

static const NYA_HttpRoute SECRET_ROUTES[] = {
    { .method             = NYA_HTTP_METHOD_GET,
     .path               = "/api/secret",
     .auth               = NYA_HTTP_AUTH_BEARER,
     .scope              = NYA_HTTP_SCOPE_READ,
     .handler_identified = secret_read,
     .summary            = "A read behind a bearer token",
     .statuses           = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

static const NYA_HttpRouter SECRET_ROUTER = {
    .name        = "secret",
    .routes      = SECRET_ROUTES,
    .route_count = nya_carray_length(SECRET_ROUTES),
};

/* BUILDING A REQUEST BY HAND: the parser has its own test; these tests are about what comes after it */

static NYA_HttpRequest* request_make(NYA_Arena* arena, NYA_HttpMethod method, NYA_ConstCString target) {
    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    *request = (NYA_HttpRequest){ .method = method, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(target, strlen(target), &request->target, &failure), "while building a request");

    (void)
        snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);

    return request;
}

static void request_header(NYA_HttpRequest* request, NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(request->header_count < NYA_HTTP_MAX_HEADERS);

    NYA_HttpHeader* header = &request->headers[request->header_count];

    // The parser lowercases every header name on the way in, and nya_http_request_header lowercases only the name it is asked for, so a stored name
    // that is not already lowercase is never found. Match the parser here.
    u64 length = 0;
    for (; name[length] != '\0' && length + 1 < sizeof(header->name); length++) {
        char c               = name[length];
        header->name[length] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    header->name[length] = '\0';

    (void)snprintf(header->value, sizeof(header->value), "%s", value);
    request->header_count++;
}

/** The `Cookie: __Host-session=<token>` a browser sends back on every request to the origin. */
static void request_session_cookie(NYA_Arena* arena, NYA_HttpRequest* request, NYA_ConstCString token) {
    request_header(request, "Cookie", nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s=%s", NYA_HTTP_SESSION_COOKIE, token)));
}

/** One dispatch through both routers, with the server's secret and a fixed clock. Returns the status answered. */
static NYA_HttpStatus run(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { &NOTE_ROUTER, &SECRET_ROUTER };

    NYA_HttpExchange exchange = {
        .request     = request,
        .response    = response,
        .arena       = arena,
        .secret      = SECRET,
        .secret_size = SECRET_SIZE,
        .now_s       = NOW_S,
        .address     = ADDRESS,
    };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

/** A note owned by `owner`, inserted, its assigned id returned. */
static s64 insert_note(u64 owner, NYA_ConstCString text) {
    SecNote note = { .owner = (s64)owner, .written_at_s = NOW_S };
    (void)snprintf(note.text, sizeof(note.text), "%s", text);

    NYA_EXPECT(nya_orm_insert(NOTES, &note), "while seeding a note");
    return note.id;
}

/* MAIN */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_http_security");
    defer      nya_arena_destroy(arena);

    // A clean throttle and a clean in-memory database: every account and session below starts from nothing.
    nya_account_throttle_reset();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    NYA_EXPECT(nya_accounts_open(arena, db));
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_EXPECT(nya_orm_open(arena, db, &SEC_NOTE_MODEL, "notes", &NOTES));
    defer nya_orm_close(NOTES);
    NYA_EXPECT(nya_orm_schema_migrate(NOTES));

    // The accounts under test. Argon2id is deliberately expensive, so these are made once and reused.
    NYA_AccountUser alice = { 0 }, bob = { 0 }, ada = { 0 }, cara = { 0 };
    NYA_EXPECT(nya_account_create(arena, "alice", PASSWORD, &alice));
    NYA_EXPECT(nya_account_create(arena, "bob", PASSWORD, &bob));
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));
    NYA_EXPECT(nya_account_create(arena, "cara", PASSWORD, &cara));

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    // ─────────────────────────── AUTHZ / IDOR ───────────────────────────
    // A note belongs to the account that wrote it. Reading or deleting another's by substituting its id
    // answers 404 — never the data, and never a 403 that would confirm the id exists.
    {
        s64 alice_note = insert_note(alice.id, "alice's private note");
        s64 bob_note   = insert_note(bob.id, "bob's private note");

        NYA_AccountSession alice_session = { 0 };
        NYA_EXPECT(nya_account_session_issue(arena, alice.id, ADDRESS, AGENT, &alice_session));

        // alice reads her own note: 200, and its text.
        NYA_HttpRequest* mine = request_make(
            arena,
            NYA_HTTP_METHOD_GET,
            nya_string_to_cstring(arena, nya_string_sprintf(arena, "/api/note?id=%lld", (long long)alice_note))
        );
        request_session_cookie(arena, mine, alice_session.token);
        nya_check(run(arena, mine, &response) == NYA_HTTP_STATUS_OK, "alice can read her own note");
        nya_check(nya_string_contains((const char*)response.body, "alice's private note"), "and gets its text back");

        // alice reaches for bob's note by its id: 404, and no trace of the text.
        NYA_HttpRequest* theirs = request_make(
            arena,
            NYA_HTTP_METHOD_GET,
            nya_string_to_cstring(arena, nya_string_sprintf(arena, "/api/note?id=%lld", (long long)bob_note))
        );
        request_session_cookie(arena, theirs, alice_session.token);
        nya_check(run(arena, theirs, &response) == NYA_HTTP_STATUS_NOT_FOUND, "alice cannot read bob's note; it is not found, not forbidden");
        nya_check(!nya_string_contains((const char*)response.body, "bob's private note"), "and none of bob's data is handed over");

        // alice deletes bob's note by id: 404, and bob's note is untouched.
        NYA_HttpRequest* wipe = request_make(
            arena,
            NYA_HTTP_METHOD_DELETE,
            nya_string_to_cstring(arena, nya_string_sprintf(arena, "/api/note?id=%lld", (long long)bob_note))
        );
        request_session_cookie(arena, wipe, alice_session.token);
        request_header(wipe, "Host", "127.0.0.1");
        nya_check(run(arena, wipe, &response) == NYA_HTTP_STATUS_NOT_FOUND, "alice cannot delete bob's note");

        SecNote survived = { 0 };
        nya_check(nya_orm_find(NOTES, arena, nya_sql_s64(bob_note), &survived).ok && survived.owner == (s64)bob.id, "bob's note is still bob's");

        // the owner-filtered query only ever counts the caller's rows.
        NYA_HttpRequest* count = request_make(arena, NYA_HTTP_METHOD_QUERY, "/api/notes");
        request_session_cookie(arena, count, alice_session.token);
        nya_check(run(arena, count, &response) == NYA_HTTP_STATUS_OK, "alice can count her notes");
        nya_check(
            nya_string_contains((const char*)response.body, "\"count\":1"),
            "and sees only her own one, not bob's, got '%s'",
            (const char*)response.body
        );
    }

    // ─────────────────────────── AUTH BYPASS ───────────────────────────
    // A protected route with no credential, a garbage credential, a revoked one, or (for the bearer
    // route) a tampered or expired token is 401/403 — never the resource.
    {
        s64 note = insert_note(alice.id, "guarded");

        NYA_ConstCString target = nya_string_to_cstring(arena, nya_string_sprintf(arena, "/api/note?id=%lld", (long long)note));

        // no cookie at all.
        nya_check(run(arena, request_make(arena, NYA_HTTP_METHOD_GET, target), &response) == NYA_HTTP_STATUS_UNAUTHORIZED, "no session is nobody");

        // a made-up cookie value.
        NYA_HttpRequest* garbage = request_make(arena, NYA_HTTP_METHOD_GET, target);
        request_session_cookie(arena, garbage, "not-a-real-token-just-a-guess");
        nya_check(run(arena, garbage, &response) == NYA_HTTP_STATUS_UNAUTHORIZED, "a guessed session token is refused");

        // a real token whose session was revoked (logout, or "log me out everywhere").
        NYA_AccountSession revoked = { 0 };
        NYA_EXPECT(nya_account_session_issue(arena, alice.id, ADDRESS, AGENT, &revoked));
        NYA_EXPECT(nya_account_session_revoke(arena, revoked.id));

        NYA_HttpRequest* stale = request_make(arena, NYA_HTTP_METHOD_GET, target);
        request_session_cookie(arena, stale, revoked.token);
        nya_check(run(arena, stale, &response) == NYA_HTTP_STATUS_UNAUTHORIZED, "a revoked session is refused, cookie and all");

        // the bearer route: a valid token opens it, and no other shape does.
        char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };

        NYA_HttpIdentity good = { .scope = NYA_HTTP_SCOPE_READ, .issued_at_s = NOW_S, .expires_at_s = NOW_S + 3600 };
        (void)snprintf(good.subject, sizeof(good.subject), "alice");
        NYA_EXPECT(nya_http_jwt_encode(&good, SECRET, SECRET_SIZE, token, sizeof(token)));

        NYA_HttpRequest* authed = request_make(arena, NYA_HTTP_METHOD_GET, "/api/secret");
        request_header(authed, "Authorization", nya_string_to_cstring(arena, nya_string_sprintf(arena, "Bearer %s", token)));
        nya_check(run(arena, authed, &response) == NYA_HTTP_STATUS_OK, "a valid bearer token opens the route");
        nya_check(nya_string_contains((const char*)response.body, "top secret"), "and reaches the resource");

        // no Authorization header.
        NYA_HttpStatus none = run(arena, request_make(arena, NYA_HTTP_METHOD_GET, "/api/secret"), &response);
        nya_check(none == NYA_HTTP_STATUS_UNAUTHORIZED || none == NYA_HTTP_STATUS_FORBIDDEN, "no token is 401/403, got %d", (s32)none);
        nya_check(!nya_string_contains((const char*)response.body, "top secret"), "and the resource stays hidden");

        // a token with one character of the signature flipped.
        NYA_String* tampered_token                        = nya_string_from(arena, token);
        tampered_token->items[tampered_token->length - 5] = tampered_token->items[tampered_token->length - 5] == 'A' ? 'B' : 'A';

        NYA_HttpRequest* tampered = request_make(arena, NYA_HTTP_METHOD_GET, "/api/secret");
        request_header(
            tampered,
            "Authorization",
            nya_string_to_cstring(arena, nya_string_sprintf(arena, "Bearer %s", nya_string_to_cstring(arena, tampered_token)))
        );
        NYA_HttpStatus tampered_status = run(arena, tampered, &response);
        nya_check(
            tampered_status == NYA_HTTP_STATUS_UNAUTHORIZED || tampered_status == NYA_HTTP_STATUS_FORBIDDEN,
            "a tampered token is 401/403, got %d",
            (s32)tampered_status
        );
        nya_check(!nya_string_contains((const char*)response.body, "top secret"), "and reaches nothing");

        // an expired token.
        char             expired[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
        NYA_HttpIdentity old = { .scope = NYA_HTTP_SCOPE_READ, .issued_at_s = NOW_S - 7200, .expires_at_s = NOW_S - 3600 };
        (void)snprintf(old.subject, sizeof(old.subject), "alice");
        NYA_EXPECT(nya_http_jwt_encode(&old, SECRET, SECRET_SIZE, expired, sizeof(expired)));

        NYA_HttpRequest* stale_bearer = request_make(arena, NYA_HTTP_METHOD_GET, "/api/secret");
        request_header(stale_bearer, "Authorization", nya_string_to_cstring(arena, nya_string_sprintf(arena, "Bearer %s", expired)));
        NYA_HttpStatus expired_status = run(arena, stale_bearer, &response);
        nya_check(
            expired_status == NYA_HTTP_STATUS_UNAUTHORIZED || expired_status == NYA_HTTP_STATUS_FORBIDDEN,
            "an expired token is 401/403, got %d",
            (s32)expired_status
        );
    }

    // ─────────────────────────── CSRF ───────────────────────────
    // A write from another site is refused before any layer or handler runs — the cookie a browser sends
    // automatically is not enough. A same-origin write, and a request with no browser origin at all, pass.
    {
        s64 note = insert_note(alice.id, "target of a forged post");

        NYA_AccountSession session = { 0 };
        NYA_EXPECT(nya_account_session_issue(arena, alice.id, ADDRESS, AGENT, &session));

        NYA_ConstCString target = nya_string_to_cstring(arena, nya_string_sprintf(arena, "/api/note?id=%lld", (long long)note));

        // a cross-site DELETE whose Origin names another host: 403, though the cookie is valid and present.
        NYA_HttpRequest* forged_origin = request_make(arena, NYA_HTTP_METHOD_DELETE, target);
        request_session_cookie(arena, forged_origin, session.token);
        request_header(forged_origin, "Host", "127.0.0.1");
        request_header(forged_origin, "Origin", "http://evil.example");
        nya_check(run(arena, forged_origin, &response) == NYA_HTTP_STATUS_FORBIDDEN, "a cross-origin write is refused");

        // a request the browser itself labels cross-site: 403, again with the cookie present.
        NYA_HttpRequest* forged_fetch = request_make(arena, NYA_HTTP_METHOD_DELETE, target);
        request_session_cookie(arena, forged_fetch, session.token);
        request_header(forged_fetch, "Host", "127.0.0.1");
        request_header(forged_fetch, "Sec-Fetch-Site", "cross-site");
        nya_check(run(arena, forged_fetch, &response) == NYA_HTTP_STATUS_FORBIDDEN, "a Sec-Fetch-Site: cross-site write is refused");

        // both refusals left the note alone.
        SecNote survived = { 0 };
        nya_check(nya_orm_find(NOTES, arena, nya_sql_s64(note), &survived).ok, "the forged writes changed nothing");

        // a same-origin write goes through.
        NYA_HttpRequest* same = request_make(arena, NYA_HTTP_METHOD_DELETE, target);
        request_session_cookie(arena, same, session.token);
        request_header(same, "Host", "127.0.0.1");
        request_header(same, "Origin", "http://127.0.0.1");
        nya_check(run(arena, same, &response) == NYA_HTTP_STATUS_NO_CONTENT, "a same-origin write is allowed");
        nya_check(!nya_orm_find(NOTES, arena, nya_sql_s64(note), &survived).ok, "and it took effect");
    }

    // ─────────────────────────── INJECTION THROUGH THE ORM ───────────────────────────
    // Attacker-controlled text is bound to a '?', never formatted into SQL, so a `'; DROP` payload is
    // stored and matched as data. A canary row planted first proves nothing was ever executed.
    {
        s64 canary = insert_note(ada.id, "canary: this row must survive");

        const char* payload = "Robert'); DROP TABLE notes;--";

        SecNote evil = { .owner = (s64)ada.id, .written_at_s = NOW_S };
        (void)snprintf(evil.text, sizeof(evil.text), "%s", payload);
        nya_check(nya_orm_insert(NOTES, &evil).ok, "an injection payload stores as an ordinary value");

        // read it back through a bound WHERE whose parameter is the same payload: still just a lookup.
        void* rows  = nullptr;
        u32   count = 0;
        nya_check(
            nya_orm_select(NOTES, arena, "WHERE text = ?", (NYA_SqlValue[]){ nya_sql_text(payload) }, 1, &rows, &count).ok,
            "a bound lookup runs"
        );
        nya_check(count == 1, "and matches exactly the row that holds the payload, got %u", count);
        if (count == 1) {
            const SecNote* got = nya_orm_at(NOTES, rows, 0);
            nya_check(nya_string_equals(got->text, payload), "stored byte for byte, not interpreted, got '%s'", got->text);
        }

        // the table still exists and the canary is still in it: no DROP ran.
        SecNote still = { 0 };
        nya_check(nya_orm_find(NOTES, arena, nya_sql_s64(canary), &still).ok, "the canary row survives, so the payload was never executed as SQL");

        // and at the raw SQL layer the same holds: a bound parameter cannot become a statement.
        NYA_SqlResult result = { 0 };
        nya_check(
            nya_sql_query(
                db,
                arena,
                "SELECT count(*) FROM notes WHERE text = ?",
                (NYA_SqlValue[]){ nya_sql_text("x'; DROP TABLE notes;--") },
                1,
                &result
            )
                .ok,
            "a bound raw query runs with a hostile parameter"
        );
        nya_check(nya_orm_find(NOTES, arena, nya_sql_s64(canary), &still).ok, "and the table is still there afterwards");
    }

    // ─────────────────────────── SESSION FIXATION ───────────────────────────
    // A session id is minted server-side and cannot be pinned by the client; it is rotated on refresh;
    // reuse of a rotated-away token is treated as theft; and a privilege change ends every prior session.
    {
        NYA_AccountSession probe = { 0 };

        // a value an attacker chose before login is not a session — only issuing makes one.
        nya_check(
            !nya_account_session_validate(arena, "a-value-an-attacker-fixed-in-advance", &probe).ok,
            "a pre-auth, client-chosen token is nobody"
        );

        // two logins never share a token, and the token is long and random, not a guessable counter.
        NYA_AccountSession first = { 0 }, second = { 0 };
        NYA_EXPECT(nya_account_session_issue(arena, alice.id, ADDRESS, AGENT, &first));
        NYA_EXPECT(nya_account_session_issue(arena, alice.id, ADDRESS, AGENT, &second));
        nya_check(!nya_string_equals(first.token, second.token), "each login mints a fresh token");
        nya_check(strlen(first.token) >= 40, "and the token is long, not a small predictable id, len %zu", strlen(first.token));

        // rotation issues a new token for the same session; the new one is live.
        NYA_AccountSession rotated = { 0 };
        NYA_EXPECT(nya_account_session_rotate(arena, first.token, ADDRESS, AGENT, &rotated));
        nya_check(!nya_string_equals(rotated.token, first.token), "rotation retires the old token for a new one");
        nya_check(nya_account_session_validate(arena, rotated.token, &probe).ok, "the rotated-to token is live");

        // reusing the rotated-away token is theft: it is refused, and the whole session is revoked, so
        // even the legitimate rotated token stops working. The real user is logged out; the thief's copy dies too.
        nya_check(!nya_account_session_rotate(arena, first.token, ADDRESS, AGENT, &probe).ok, "reusing a rotated-away token is refused");
        nya_check(!nya_account_session_validate(arena, rotated.token, &probe).ok, "and the reuse revokes the whole session");

        // a privilege change — here a password change — ends every session the account held.
        NYA_AccountSession before_change = { 0 };
        NYA_EXPECT(nya_account_session_issue(arena, cara.id, ADDRESS, AGENT, &before_change));
        nya_check(nya_account_session_validate(arena, before_change.token, &probe).ok, "cara has a live session before the change");

        NYA_EXPECT(nya_account_password_change(arena, cara.id, PASSWORD, REPLACEMENT));
        nya_check(!nya_account_session_validate(arena, before_change.token, &probe).ok, "changing the password ends every session issued before it");
    }

    // ─────────────────────────── HEADER / RESPONSE SPLITTING ───────────────────────────
    // CRLF cannot enter through a reflected value: the URL parser refuses a target that decodes to a
    // control byte, and the response writer refuses a CR or LF in a header name, a header value, or a
    // cookie value — so a value that somehow carried one still cannot open a second header or a body.
    {
        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };

        // the path is one place the parser itself stops a decoded control byte.
        nya_check(
            !nya_url_parse_target("/api/%0d%0abad", strlen("/api/%0d%0abad"), &url, &failure).ok,
            "a path that decodes to CRLF is refused at the parser"
        );

        // the query is not: a value there may decode to anything but NUL. So the guard that matters is the
        // response writer, reached here end to end — a handler reflects ?next into a header, and a value
        // that decoded a CRLF is refused, answering 400 rather than emitting a second header.
        NYA_HttpRequest* inject = request_make(arena, NYA_HTTP_METHOD_GET, "/api/echo?next=x%0d%0aSet-Cookie:%20pwned=1");
        nya_check(run(arena, inject, &response) == NYA_HTTP_STATUS_BAD_REQUEST, "a reflected value carrying CRLF is refused, not emitted");
        nya_check(!nya_string_contains((const char*)response.body, "pwned"), "and nothing injected reached the response");

        // a clean value reflects fine, so the refusal above is about the CRLF and not the reflection.
        NYA_HttpRequest* clean = request_make(arena, NYA_HTTP_METHOD_GET, "/api/echo?next=a-clean-value");
        nya_check(run(arena, clean, &response) == NYA_HTTP_STATUS_OK, "a clean reflected value is allowed");

        NYA_HttpResponse splitting           = { 0 };
        u8               splitting_body[256] = { 0 };
        nya_http_response_create(&splitting, splitting_body, sizeof(splitting_body));
        defer nya_http_response_destroy(&splitting);

        nya_check(nya_http_response_text(&splitting, "ok", NYA_HTTP_MEDIA_TEXT).ok, "a body is written");

        // the second line of defence: a CR or LF in either half of a header is refused, not stripped.
        nya_check(!nya_http_response_header(&splitting, "X-Reflected", "value\r\nSet-Cookie: pwned=1").ok, "a CRLF in a header value is refused");
        nya_check(!nya_http_response_header(&splitting, "X-Bad\r\nInjected", "value").ok, "a CRLF in a header name is refused");
        nya_check(nya_http_response_header(&splitting, "X-Reflected", "a-clean-value").ok, "a clean value is accepted");

        // a cookie value carrying CRLF is refused the same way.
        nya_check(
            !nya_http_response_cookie(&splitting, &(NYA_HttpCookie){ .name = "session", .value = "abc\r\nSetCookie=pwned", .path = "/" }).ok,
            "a CRLF in a cookie value is refused"
        );

        // and the rendered head carries none of the injected material.
        u8  head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
        u64 head_size                              = 0;
        nya_check(
            nya_http_response_head(&splitting, NYA_HTTP_STATUS_OK, true, (NYA_Instant){ 0 }, head, sizeof(head), &head_size).ok,
            "the head renders"
        );

        NYA_String* rendered = nya_string_from(arena, (NYA_ConstCString)head);
        nya_check(nya_string_contains(rendered, "X-Reflected: a-clean-value\r\n"), "the clean header is present");
        nya_check(!nya_string_contains(rendered, "pwned"), "and nothing injected reached the head");
        nya_check(!nya_string_contains(rendered, "Injected"), "no forged header name either");
    }

    if (nya_check_failures() == 0) printf("PASSED: http security\n");

    return nya_check_failures() == 0 ? 0 : 1;
}
