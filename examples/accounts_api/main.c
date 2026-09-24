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
 * curl -b jar localhost:47810/api/session
 * curl -b jar -X POST  localhost:47810/api/notes -d '{"text":"my first note"}'
 * curl -b jar -X QUERY localhost:47810/api/notes -d '{}'          # only ada's notes
 * curl -b jar -X DELETE localhost:47810/api/notes -d '{"id":1}'
 * curl -b jar localhost:47810/api/sessions                        # signed-in devices
 * curl -b jar -X POST localhost:47810/api/logout
 * ```
 *
 * ## The login flow is a mounted module, not code in this file
 *
 * Register, log in, log out, the second factor, and the who-am-I check are one router the `accounts`
 * module exposes: nya_http_accounts_open hands back an NYA_HttpRouter this example merges, exactly as it
 * merges its own notes and sessions routers. What used to be some eight hundred lines of handler here —
 * the password dance, the session cookie, the sealed pending-login cookie, the TOTP enrol/verify pair —
 * lives in src/nyangine/http/http_accounts.c now, wired to the same primitives once. This file is what
 * is left when the reusable half is lifted out: a config, a mount, and the two resources that are
 * genuinely this example's — notes, which belong to people, and the signed-in-devices list.
 *
 * ## The second factor: a login in two steps
 *
 * An account may enrol a TOTP authenticator, after which its login is two requests. The first checks the
 * password and, finding a second factor, answers `{"second_factor_required": true}` and a sealed
 * `__Host-login` cookie instead of a session; the second answers a code and gets the session.
 *
 * ```
 * curl -b jar -c jar -X POST localhost:47810/api/totp/enrol     # returns the otpauth URI, the secret, recovery codes
 * curl -b jar -c jar -X POST localhost:47810/api/totp/confirm -d '{"code":"123456"}'   # one code turns it on
 *
 * curl -c jar -X POST localhost:47810/api/login -d '{"username":"ada","password":"a long passphrase"}'   # -> second_factor_required
 * curl -b jar -c jar -X POST localhost:47810/api/login/totp -d '{"code":"123456"}'     # -> the session cookie
 * ```
 *
 * A recovery code off paper is accepted at `/api/login/totp` in place of a code, and spent when it is.
 *
 * ## Passkeys: a passwordless login, not a second step
 *
 * Because this mount names a WebAuthn relying party (`passkey_rp_id` and `passkey_origin`), the accounts
 * router also carries four `/api/passkey/…` routes: a passkey is a *primary* way to log in here, not only a
 * second factor. An account can be created with a passkey and never hold a password, and one that has a
 * password can add a passkey and log in with either.
 *
 * ```
 * POST /api/passkey/register/begin   # signed out: opens a passwordless account, returns the create options
 * POST /api/passkey/register/finish  # stores the credential; on the signed-out path, logs the new account in
 * POST /api/passkey/login/begin      # {"username":"ada"} -> an assertion challenge and ada's credentials
 * POST /api/passkey/login/finish     # the browser's assertion -> the __Host-session cookie, no password
 * ```
 *
 * The begin routes answer the JSON `navigator.credentials.create`/`.get` need — the challenge, the relying
 * party id, and (on login) the account's credential ids; the browser signs, and the finish routes hand the
 * clientDataJSON, attestationObject / authenticatorData and signature back as base64url. There is no curl
 * transcript for it the way there is for the password flow: the signing is the authenticator's, so it is a
 * browser or a test with a key of its own (see tests/nyangine/accounts/test_passkey_login_flow.c) that
 * drives it. The session cookie the login sets is the very one the password login sets, with the same flags.
 *
 * ## The thing worth reading for: notes belong to people
 *
 * Every note has an `owner`. A note is only ever shown to, or deleted by, the account that wrote it —
 * `notes_query` filters on the owner and `notes_delete` refuses an id that is not the caller's. That is
 * the check a scanner probes for as IDOR: log in as one user, ask for another user's note by id, and a
 * server that answers it has handed one person another's data. Here the answer is 404, the same as for
 * an id that does not exist, because whether somebody else's note exists is not the caller's business
 * either. The caller behind every one of these checks is read with nya_http_accounts_caller, the same
 * cookie validation the mounted `/api/session` route uses.
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

// The notes resource, split into its three shapes and their conversions — the worked example of
// "Model, SO, DTO". note_so.h pulls note_model.h (the row) and note_dto.h (the wire), and holds the
// four conversions between them. Only note_dto.h would compile into the web profile; the other two
// carry the guard that refuses to.
#include "notes/note_so.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS AND STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define DEFAULT_PORT 47810

/** Notes one account may hold, so the file cannot grow without bound. */
#define NOTES_PER_USER 1000

/** The program's name, as it appears in the authenticator's list beside the account. */
#define TOTP_ISSUER "accounts_api"

/**
 * The WebAuthn relying party the passkey routes bind every credential to.
 *
 * The RP id is the effective domain a browser scopes the credential to; `localhost` is the one host a
 * browser lets WebAuthn run on without TLS, so the example works over plain http. The origin is the RP id
 * with the scheme and port, and the passkey routes check the browser's clientDataJSON against it exactly.
 * A deployed server sets these to its real domain and https origin; here the origin is built from the port
 * the example is actually listening on, so the check matches whatever `--port` chose.
 * */
#define PASSKEY_RP_ID "localhost"

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL NYA_Arena*    DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database* DB       = nullptr;
NYA_INTERNAL NYA_OrmTable* NOTES    = nullptr;

/**
 * The key the accounts routes seal their "password accepted, second factor still owed" cookie with.
 *
 * A fresh random key made at startup, not the token signing secret and not from the environment: the
 * pending-login cookie lives five minutes and never has to survive a restart, so a per-process key is
 * exactly enough and keeps the example runnable with nothing to configure. It is handed to
 * nya_http_accounts_open as the login seal seam; see http_accounts.h and http_seal.h.
 * */
NYA_INTERNAL u8 LOGIN_SEAL_SECRET[32] = { 0 };

#define NOTES_PATH    "/api/notes"
#define SESSIONS_PATH "/api/sessions"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: NOTES, WHICH BELONG TO PEOPLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One stored row as the JSON the client sees — the DTO, rendered through the DTO's own reflection.
 *
 * The row goes Model → SO → DTO before it is written, so `owner` falls away where the DTO has no field
 * for it: the owner is not the reader's to see. The wire shape is decided by NOTE_DTO_V1_REFLECT and
 * nothing else, exactly as the ORM's shape is decided by NOTE_MODEL.
 * */
NYA_INTERNAL NYA_Value note_dto_value(NYA_Arena* arena, const AccountNote* row) {
    Note      so  = note_so_from_model(row);
    NoteDtoV1 dto = note_dto_from_so(&so);

    NYA_Object* object = nya_reflect_to_object(arena, &NOTE_DTO_V1_REFLECT, &dto);
    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

/** The caller's own notes, and nobody else's. The filter is on the owner, in the query. */
NYA_INTERNAL NYA_HttpStatus handle_notes_query(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    void* rows  = nullptr;
    u32   count = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ? ORDER BY id", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &rows, &count).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Object*          body  = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* notes = nya_array_create(exchange->arena, NYA_Value);

    for (u32 index = 0; index < count; index++) {
        const AccountNote* note = nya_orm_at(NOTES, rows, index);

        NYA_Value value = note_dto_value(exchange->arena, note);
        nya_array_add(notes, value);
    }

    nya_object_add(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = notes->length });
    nya_object_add(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Adds a note owned by the caller. */
NYA_INTERNAL NYA_HttpStatus handle_notes_post(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // The request body is read as the DTO through its reflection, then parsed into an SO — which is
    // where the untrusted text is checked and where the *server*, not the client, fills in the owner
    // and the timestamp. A client cannot claim a note it did not write: note_so_from_dto ignores any
    // owner a DTO might carry, because the DTO has no such field to carry.
    NoteDtoV1 dto = { 0 };
    if (!nya_http_request_reflect(exchange->request, exchange->arena, &NOTE_DTO_V1_REFLECT, &dto).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    Note so = { 0 };
    if (!note_so_from_dto(&dto, (s64)user.id, exchange->now_s, &so).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    void* existing = nullptr;
    u32   held     = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &existing, &held).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (held >= NOTES_PER_USER) return NYA_HTTP_STATUS_UNPROCESSABLE;

    AccountNote note = note_model_from_so(&so);
    if (!nya_orm_insert(NOTES, &note).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR; // writes the assigned id back into note.

    // The stored row back out as the DTO, so the client reads exactly what a later query would return.
    Note      stored_so  = note_so_from_model(&note);
    NoteDtoV1 stored_dto = note_dto_from_so(&stored_so);

    return nya_http_response_reflect(exchange->response, exchange->arena, &NOTE_DTO_V1_REFLECT, &stored_dto).ok ? NYA_HTTP_STATUS_CREATED
                                                                                                                : NYA_HTTP_STATUS_INTERNAL_ERROR;
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
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

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
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

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
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

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
 *
 * Only the two resources that are this example's own. Register, login, logout, the second factor and the
 * session check are the accounts module's router, merged in main; see nya_http_accounts_open.
 */

/*
 * Every handler touches the one database on the ticking thread, so the whole table is MAIN: no route
 * here runs on a worker, and two requests never race the same rows.
 */
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

    // The key the accounts routes seal their pending-login cookie with. Fresh at startup; see the field.
    if (!nya_os_random_bytes(LOGIN_SEAL_SECRET, sizeof(LOGIN_SEAL_SECRET))) {
        nya_log_error("No entropy for the pending-login key, so the second factor cannot be sealed.");
        return EXIT_FAILURE;
    }
    defer nya_memset(LOGIN_SEAL_SECRET, 0, sizeof(LOGIN_SEAL_SECRET));

    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
                   .port             = port,
                   .workers          = 2,
                   .certificate_path = certificate_path,
                   .key_path         = key_path,
               }),
               "while starting the server");
    defer nya_system_http_deinit();

    // The origin the passkey routes check against: the scheme, the host, and the port actually chosen.
    // Built here rather than hardcoded so it matches whatever `--port` picked; a real server would name its
    // own https domain instead. localhost is the one host a browser runs WebAuthn on without TLS.
    char passkey_origin[64] = { 0 };
    (void)snprintf(passkey_origin, sizeof(passkey_origin), "%s://%s:%u", secure ? "https" : "http", PASSKEY_RP_ID, nya_http_server_port());

    // The login flow, mounted from the accounts module: register, login, login/totp, logout, session, the
    // TOTP enrol/confirm pair, and — because a relying party is named — the passwordless passkey routes.
    // Everything this example does not have to write itself.
    const NYA_HttpRouter* accounts = nya_http_accounts_open((NYA_HttpAccountsConfig){
        .arena                  = DB_ARENA,
        .database               = DB,
        .registration           = NYA_ACCOUNT_REGISTRATION_OPEN,
        .totp_issuer            = TOTP_ISSUER,
        .login_seal_secret      = LOGIN_SEAL_SECRET,
        .login_seal_secret_size = sizeof(LOGIN_SEAL_SECRET),
        .passkey_rp_id          = PASSKEY_RP_ID,
        .passkey_origin         = passkey_origin,
    });
    if (accounts == nullptr) {
        nya_log_error("Could not mount the accounts routes.");
        return EXIT_FAILURE;
    }
    defer nya_http_accounts_close();

    NYA_EXPECT(nya_http_server_merge(accounts), "while mounting the accounts routes");
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
