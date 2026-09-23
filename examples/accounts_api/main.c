/**
 * @file examples/accounts_api/main.c
 *
 * The accounts module over HTTP: register, log in, a session in a cookie, and notes that belong to the
 * person who wrote them.
 *
 * ```
 * ./build run example accounts_api                 # serves on 127.0.0.1:47810 until interrupted
 * ./accounts_api.example --port 8080
 * ./accounts_api.example --certificate cert.pem --key key.pem   # over https
 * ```
 *
 * From another terminal (a cookie jar is how a browser keeps the session):
 *
 * ```
 * curl -c jar -X POST localhost:47810/api/register -d '{"username":"ada","password":"a long passphrase"}'
 * curl -c jar -b jar -X POST localhost:47810/api/login -d '{"username":"ada","password":"a long passphrase"}'
 * curl -b jar localhost:47810/api/me
 * curl -b jar -X POST  localhost:47810/api/notes -d '{"text":"my first note"}'
 * curl -b jar -X QUERY localhost:47810/api/notes -d '{}'          # only ada's notes
 * curl -b jar -X DELETE localhost:47810/api/notes -d '{"id":1}'
 * curl -b jar localhost:47810/api/sessions                        # signed-in devices
 * curl -b jar -X POST localhost:47810/api/logout
 * ```
 *
 * ## What this example is for
 *
 * `web_server` shows the router, the DTOs and the OpenAPI document with no user store — its second
 * factor "stands for both", as its own comment says. This one is the other half: a real `accounts`
 * database, a real login, a session that is a row rather than a signed claim, and authorization that
 * knows who owns what.
 *
 * ## The thing worth reading for: notes belong to people
 *
 * Every note has an `owner`. A note is only ever shown to, or deleted by, the account that wrote it —
 * `notes_query` filters on the owner and `notes_delete` refuses an id that is not the caller's. That is
 * the check a scanner probes for as IDOR: log in as one user, ask for another user's note by id, and a
 * server that answers it has handed one person another's data. Here the answer is 404, the same as for
 * an id that does not exist, because whether somebody else's note exists is not the caller's business
 * either.
 *
 * ## The session is a row, not a claim
 *
 * Logging in issues an `accounts` session — an opaque token, hashed in the database — and puts it in a
 * `__Host-session` cookie: `HttpOnly`, `Secure`, `SameSite=Strict`, `Path=/`. Every request validates
 * it against the row, so revoking a session (logging out, or from the devices list) ends it at once,
 * which a signed token living minutes cannot do. The throttle in `accounts` slows a password-guessing
 * login the same way whoever is guessing gets slowed.
 *
 * ## Not encrypted on disk
 *
 * The database is under the save root and is not encrypted (see db.h): the password hashes in it are
 * Argon2id and safe to leak, but do not put anything else here that would matter if somebody read the
 * file until SQLCipher is vendored.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS AND STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define DEFAULT_PORT 47810

/** Longest note. Small: this is a demonstration of ownership, not a document store. */
#define NOTE_TEXT_MAX 280

/** Notes one account may hold, so the file cannot grow without bound. */
#define NOTES_PER_USER 1000

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL NYA_Arena*    DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database* DB       = nullptr;
NYA_INTERNAL NYA_OrmTable* NOTES    = nullptr;

/** One note and who owns it. `owner` is the account id; nothing is ever read across owners. */
typedef struct {
    s64  id;
    s64  owner;
    s64  written_at_s;
    char text[NOTE_TEXT_MAX];
} AccountNote;

/*
 * The description the ORM builds the table from, written by hand: the reflection pass scans only
 * src/nyangine and src/gnyame, so a type declared in an example has no generated table and
 * nya_reflect_of does not resolve for it. Inside the engine these would be one `// @reflect` comment.
 */
NYA_INTERNAL const NYA_TypeReflection NOTE_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NOTE_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NOTE_TEXT_MAX,
};

NYA_INTERNAL const NYA_ReflectField NOTE_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, id), .is_key = true },
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, owner) },
    { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, written_at_s) },
    { .name = "text", .type = &NOTE_TEXT_ARRAY, .offset = nya_offsetof(AccountNote, text) },
};

NYA_INTERNAL const NYA_TypeReflection NOTE_MODEL = {
    .name        = "AccountNote",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(AccountNote),
    .alignment   = alignof(AccountNote),
    .fields      = NOTE_FIELDS,
    .field_count = nya_carray_length(NOTE_FIELDS),
};

#define REGISTER_PATH "/api/register"
#define LOGIN_PATH    "/api/login"
#define LOGOUT_PATH   "/api/logout"
#define ME_PATH       "/api/me"
#define NOTES_PATH    "/api/notes"
#define SESSIONS_PATH "/api/sessions"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SESSION HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The account this request's cookie names, or false with the 401 already the caller's to return.
 *
 * Reads the session token out of the `__Host-session` cookie and validates it against the row, so a
 * revoked, expired or forged cookie is nobody. The cookie value is copied into a NUL-terminated buffer
 * because the parser hands back a view that is not terminated.
 * */
NYA_INTERNAL b8 request_account(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: ACCOUNTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registration, open to anybody in this example. A taken name or a short password says which. */
NYA_INTERNAL NYA_HttpStatus handle_register(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user = { 0 };
    NYA_Error       made = nya_account_register(exchange->arena, NYA_ACCOUNT_REGISTRATION_OPEN, username, password, nullptr, &user);

    if (!made.ok) {
        // A taken name and a bad password are the caller's to fix, so they are told; anything else is a
        // server fault the caller can do nothing about.
        if (made.kind == NYA_ERROR_ALREADY_EXISTS || made.kind == NYA_ERROR_INVALID_ARGUMENT) return NYA_HTTP_STATUS_UNPROCESSABLE;

        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/** Sets the session cookie from a token, with the attributes a session cookie must have. */
NYA_INTERNAL NYA_HttpStatus set_session_cookie(NYA_HttpExchange* exchange, NYA_ConstCString token) {
    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = NYA_HTTP_SESSION_COOKIE,
                                                 .value     = token,
                                                 .max_age_s = NYA_ACCOUNTS_SESSION_IDLE_S,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                             });

    return set.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** A password, and on success a session in a cookie. The throttle in accounts slows a guessing spree. */
NYA_INTERNAL NYA_HttpStatus handle_login(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user      = { 0 };
    NYA_Error       allowed   = nya_account_authenticate(exchange->arena, username, password, request_address(exchange), &user);

    // One answer for every way it fails — wrong password, no such user, disabled, throttled — so the
    // response says nothing a guesser can use. 401, since it is the credentials that were refused.
    if (!allowed.ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_ConstCString agent = nya_http_request_header(exchange->request, "user-agent");

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_issue(exchange->arena, user.id, request_address(exchange), agent, &session).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return set_session_cookie(exchange, session.token);
}

/** Signing out: the session row is revoked, not only the cookie cleared, so the token cannot be reused. */
NYA_INTERNAL NYA_HttpStatus handle_logout(NYA_HttpExchange* exchange) {
    NYA_HttpCookieValue cookie = { 0 };

    if (nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) {
        char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };

        if (cookie.size < sizeof(token)) {
            nya_memcpy(token, cookie.text, cookie.size);

            NYA_AccountSession session = { 0 };

            // Validate to find the row, then revoke it. A cookie that is already invalid is nothing to
            // revoke, and clearing it is still the right thing to do.
            if (nya_account_session_validate(exchange->arena, token, &session).ok) {
                (void)nya_account_session_revoke(exchange->arena, session.id);
            }
        }
    }

    NYA_Error cleared = nya_http_response_cookie_clear(exchange->response, NYA_HTTP_SESSION_COOKIE, "/", true);

    return cleared.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Who the cookie says you are. 401 when it says nobody. */
NYA_INTERNAL NYA_HttpStatus handle_me(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* body = nya_object_create(exchange->arena);

    nya_object_add(body, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = user.id });
    nya_object_add(body, "username", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user.username });
    nya_object_add(body, "display", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user.display });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: NOTES, WHICH BELONG TO PEOPLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One note as a JSON value, without its owner: the owner is not the reader's to see. */
NYA_INTERNAL NYA_Value note_to_value(NYA_Arena* arena, const AccountNote* note) {
    NYA_Object* object = nya_object_create(arena);

    nya_object_add(object, "id", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = note->id });
    nya_object_add(object, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)note->text });
    nya_object_add(object, "written_at_s", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = note->written_at_s });

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

/** The caller's own notes, and nobody else's. The filter is on the owner, in the query. */
NYA_INTERNAL NYA_HttpStatus handle_notes_query(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    void* rows  = nullptr;
    u32   count = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ? ORDER BY id", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &rows, &count).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Object*          body  = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* notes = nya_array_create(exchange->arena, NYA_Value);

    for (u32 index = 0; index < count; index++) {
        const AccountNote* note = nya_orm_at(NOTES, rows, index);

        NYA_Value value = note_to_value(exchange->arena, note);
        nya_array_add(notes, value);
    }

    nya_object_add(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = notes->length });
    nya_object_add(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Adds a note owned by the caller. */
NYA_INTERNAL NYA_HttpStatus handle_notes_post(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* text = nya_object_get(incoming, "text");
    if (text == nullptr || text->type != NYA_TYPE_STRING || text->as_string[0] == '\0') return NYA_HTTP_STATUS_BAD_REQUEST;

    void* existing = nullptr;
    u32   held     = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &existing, &held).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (held >= NOTES_PER_USER) return NYA_HTTP_STATUS_UNPROCESSABLE;

    AccountNote note = { .owner = (s64)user.id, .written_at_s = (s64)exchange->now_s };
    (void)snprintf(note.text, sizeof(note.text), "%s", text->as_string);

    if (!nya_orm_insert(NOTES, &note).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Value stored = note_to_value(exchange->arena, &note);

    return nya_http_response_json(exchange->response, exchange->arena, &stored.as_object).ok ? NYA_HTTP_STATUS_CREATED : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Deletes one of the caller's notes by id — and only one of the caller's.
 *
 * This is the IDOR check. A note that exists but belongs to somebody else answers 404, the same as a
 * note that does not exist, because "that is not yours" and "there is no such note" are the same
 * sentence to somebody who has no business knowing either way.
 * */
NYA_INTERNAL NYA_HttpStatus handle_notes_delete(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id = nya_object_get(incoming, "id");
    if (id == nullptr || (id->type != NYA_TYPE_U64 && id->type != NYA_TYPE_S64)) return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 note_id = id->type == NYA_TYPE_U64 ? id->as_u64 : (u64)id->as_s64;

    AccountNote note = { 0 };
    if (!nya_orm_find(NOTES, exchange->arena, nya_sql_s64((s64)note_id), &note).ok) return NYA_HTTP_STATUS_NOT_FOUND;

    // The owner check, and the whole point of the example: not yours is the same answer as not there.
    if (note.owner != (s64)user.id) return NYA_HTTP_STATUS_NOT_FOUND;

    if (!nya_orm_delete(NOTES, nya_sql_s64((s64)note_id)).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: SIGNED-IN DEVICES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The caller's own sessions, the "signed-in devices" list. Never a token, only where and when. */
NYA_INTERNAL NYA_HttpStatus handle_sessions_list(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_AccountSession* sessions = nullptr;
    u32                 count    = 0;

    if (!nya_account_session_list(exchange->arena, user.id, &sessions, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object*          body = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* list = nya_array_create(exchange->arena, NYA_Value);

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountSession* session = &sessions[index];

        NYA_Object* row = nya_object_create(exchange->arena);

        nya_object_add(row, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->id });
        nya_object_add(row, "address", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)session->address });
        nya_object_add(row, "agent", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)session->agent });
        nya_object_add(row, "created_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->created_at_s });
        nya_object_add(row, "used_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->used_at_s });
        nya_object_add(row, "revoked", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = session->revoked });

        NYA_Value row_value = { .type = NYA_TYPE_OBJECT, .as_object = *row };
        nya_array_add(list, row_value);
    }

    nya_object_add(body, "sessions", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *list });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Revokes one of the caller's own sessions by id. Somebody else's is not found, like a note. */
NYA_INTERNAL NYA_HttpStatus handle_sessions_delete(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id = nya_object_get(incoming, "id");
    if (id == nullptr || (id->type != NYA_TYPE_U64 && id->type != NYA_TYPE_S64)) return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 session_id = id->type == NYA_TYPE_U64 ? id->as_u64 : (u64)id->as_s64;

    // The session has to be one of the caller's own: revoking by id alone would let anybody end
    // anybody's session, which is the same IDOR the notes have.
    NYA_AccountSession* sessions = nullptr;
    u32                 count    = 0;

    if (!nya_account_session_list(exchange->arena, user.id, &sessions, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    for (u32 index = 0; index < count; index++) {
        if (sessions[index].id != session_id) continue;

        return nya_account_session_revoke(exchange->arena, session_id).ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_NOT_FOUND;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROUTES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every handler touches the one database on the ticking thread, so the whole table is MAIN: no route
 * here runs on a worker, and two requests never race the same rows.
 */
NYA_INTERNAL const NYA_HttpRoute ACCOUNT_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = REGISTER_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_register,
      .summary = "Makes an account", .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = LOGIN_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login,
      .summary = "Logs in and sets the session cookie", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = LOGOUT_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_logout,
      .summary = "Revokes the session and clears the cookie", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_GET, .path = ME_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_me,
      .summary = "Who the cookie says you are", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
};

NYA_INTERNAL const NYA_HttpRouter ACCOUNT_ROUTER = {
    .name = "accounts", .routes = ACCOUNT_ROUTES, .route_count = nya_carray_length(ACCOUNT_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute NOTE_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_QUERY, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_query,
      .summary = "Your own notes", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
    { .method = NYA_HTTP_METHOD_POST, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_post,
      .summary = "Adds a note you own", .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_DELETE, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_delete,
      .summary = "Deletes a note you own", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_FORBIDDEN } },
};

NYA_INTERNAL const NYA_HttpRouter NOTE_ROUTER = {
    .name = "notes", .routes = NOTE_ROUTES, .route_count = nya_carray_length(NOTE_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute SESSION_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = SESSIONS_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_sessions_list,
      .summary = "Your signed-in devices", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
    { .method = NYA_HTTP_METHOD_DELETE, .path = SESSIONS_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_sessions_delete,
      .summary = "Revokes one of your sessions", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_FORBIDDEN } },
};

NYA_INTERNAL const NYA_HttpRouter SESSION_ROUTER = {
    .name = "sessions", .routes = SESSION_ROUTES, .route_count = nya_carray_length(SESSION_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    NYA_ConstCString certificate_path = "";
    NYA_ConstCString key_path         = "";

    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--certificate") == 0) certificate_path = argv[i + 1];
        if (strcmp(argv[i], "--key") == 0) key_path = argv[i + 1];
        if (strcmp(argv[i], "--port") != 0) continue;

        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    b8 secure = certificate_path[0] != '\0' || key_path[0] != '\0';

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();

    // No window, no renderer, no frame loop: an app instance for the systems to hang off, the callback
    // and event registries the save and http systems hook into, and then the database. See web_server.
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init(), "while starting the event registry");
    defer nya_system_events_deinit();

    NYA_Error saves = nya_system_save_init();
    if (!saves.ok) {
        nya_log_error("No save root, so there is nowhere to keep the accounts: %s", (NYA_ConstCString)saves.message);
        return EXIT_FAILURE;
    }
    defer nya_system_save_deinit();

    DB_ARENA = nya_arena_create(.name = "accounts_db");
    defer    nya_arena_destroy(DB_ARENA);

    NYA_Error stored = nya_save_database_open(DB_ARENA, "accounts.db", &DB);
    if (!stored.ok) {
        nya_log_error("Could not open the accounts database: %s", (NYA_ConstCString)stored.message);
        return EXIT_FAILURE;
    }
    defer nya_sql_close(DB);

    // The accounts module and its six tables, on this database.
    NYA_EXPECT(nya_accounts_open(DB_ARENA, DB), "while opening the accounts tables");
    defer nya_accounts_close();

    // The notes table, owned per account.
    NYA_EXPECT(nya_orm_open(DB_ARENA, DB, &NOTE_MODEL, "notes", &NOTES), "while binding the note model");
    defer nya_orm_close(NOTES);
    NYA_EXPECT(nya_orm_schema_migrate(NOTES), "while bringing the notes table level with the model");

    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
                   .port             = port,
                   .workers          = 2,
                   .certificate_path = certificate_path,
                   .key_path         = key_path,
               }),
               "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&ACCOUNT_ROUTER), "while mounting the accounts routes");
    NYA_EXPECT(nya_http_server_merge(&NOTE_ROUTER), "while mounting the notes routes");
    NYA_EXPECT(nya_http_server_merge(&SESSION_ROUTER), "while mounting the sessions routes");
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while mounting the OpenAPI document");

    nya_log_info("accounts_api on %s://127.0.0.1:%u — register, log in, and keep notes that are yours. ctrl-c to stop.",
                 secure ? "https" : "http", nya_http_server_port());

    // The housekeeping a real server runs on a timer, run once at start so a long-lived database does
    // not carry dead rows forever. A production server would call these hourly.
    u32 ended = 0, removed = 0;
    (void)nya_account_session_sweep(DB_ARENA, &ended, &removed);

    while (RUNNING) {
        nya_system_http_tick();
        nya_os_time_sleep_ms(2);
    }

    nya_log_info("Stopping.");

    return EXIT_SUCCESS;
}
