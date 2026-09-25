/**
 * The file routes end to end: a real port, a real socket, a session in a cookie, and a file that goes
 * into the blob store and comes back only to the account that put it there.
 *
 * test_multipart.c proves the parser in the small. This proves the facade over it: that an upload stores
 * the file part and answers its id, that the owner downloads it back with its content type and an
 * attachment disposition intact, and — the point of the whole thing — that a second account handed the
 * very same id is refused with 403, because an id is not a capability. The server is driven by hand: a
 * request is written, nya_system_http_tick runs the handler on this thread (every route is
 * NYA_HTTP_AFFINITY_MAIN), and the answer is read back. The database is `:memory:`.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <string.h>
#include <time.h>

#define PASSWORD "a correct horse battery staple"

/* THE LOOPBACK HARNESS: the same shape as test_login_flow.c */

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

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

/** Sends `text`, ticks the server, and reads back a whole answer: the head, then exactly its Content-Length. Returns the bytes read. */
static u64 exchange(NYA_OsSocket socket, const u8* text, u64 text_size, OUT char* buffer, u64 capacity) {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(socket, text, text_size, &wrote) == NYA_OS_SOCKET_OK && wrote == text_size);

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

/** A convenience over exchange for a request that is a C string. */
static u64 exchange_text(NYA_OsSocket socket, NYA_ConstCString text, OUT char* buffer, u64 capacity) {
    return exchange(socket, (const u8*)text, strlen(text), buffer, capacity);
}

static u32 status_of(const char* answer) {
    if (strncmp(answer, "HTTP/1.1 ", 9) != 0) return 0;
    return (u32)strtoul(answer + 9, nullptr, 10);
}

/** The body of a response, after the blank line; points into `answer`. Null when there is no head yet. */
static const char* body_of(const char* answer) {
    const char* blank = strstr(answer, "\r\n\r\n");
    return blank == nullptr ? nullptr : blank + 4;
}

/** Whether the response carries a header line `name: value` exactly (value to end of line). */
static b8 has_header(const char* answer, NYA_ConstCString name, NYA_ConstCString value) {
    NYA_String* line  = nya_string_sprintf(nya_arena_global, "\r\n%s: %s\r\n", name, value);
    b8          found = strstr(answer, nya_string_to_cstring(nya_arena_global, line)) != nullptr;
    return found;
}

/** Issues a session for a fresh account and returns its cookie token. */
static void account_with_session(NYA_Arena* arena, NYA_ConstCString username, OUT char* out_token, u64 capacity) {
    NYA_AccountUser user = { 0 };
    NYA_EXPECT(nya_account_create(arena, username, PASSWORD, &user));

    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, user.id, "127.0.0.1", "test-agent", &session));

    nya_assert(strlen(session.token) < capacity);
    (void)snprintf(out_token, capacity, "%s", session.token);
}

/** Builds a multipart/form-data upload request with one file part carrying `content`. */
static NYA_String*
upload_request(NYA_Arena* arena, NYA_ConstCString token, NYA_ConstCString filename, NYA_ConstCString content_type, NYA_ConstCString content) {
    NYA_String* multipart = nya_string_sprintf(
        arena,
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n"
        "\r\n"
        "%s\r\n"
        "--BOUNDARY--\r\n",
        filename,
        content_type,
        content
    );

    NYA_ConstCString body = nya_string_to_cstring(arena, multipart);

    return nya_string_sprintf(
        arena,
        "POST /api/files HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\n"
        "Content-Type: multipart/form-data; boundary=BOUNDARY\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
        token,
        strlen(body),
        body
    );
}

/* MAIN */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_files");
    defer      nya_arena_destroy(arena);

    nya_account_throttle_reset();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    NYA_EXPECT(nya_accounts_open(arena, db));
    defer nya_accounts_close();
    defer nya_sql_close(db);

    // The accounts routes must be mounted so nya_http_accounts_caller can resolve a session; the files routes lean on it. The seal secret is a
    // throwaway for the test.
    u8 seal[32] = { 0 };
    nya_assert(nya_os_random_bytes(seal, sizeof(seal)), "the test could not seed a seal secret");

    const NYA_HttpRouter* accounts = nya_http_accounts_open((NYA_HttpAccountsConfig){
        .arena                  = arena,
        .database               = db,
        .registration           = NYA_ACCOUNT_REGISTRATION_OPEN,
        .totp_issuer            = "test",
        .login_seal_secret      = seal,
        .login_seal_secret_size = sizeof(seal),
    });
    nya_assert(accounts != nullptr, "the accounts routes mounted");
    defer nya_http_accounts_close();

    const NYA_HttpRouter* files = nya_http_files_open((NYA_HttpFilesConfig){ .arena = arena, .database = db });
    nya_assert(files != nullptr, "the file routes mounted");
    defer nya_http_files_close();

    char ada[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    char bob[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    account_with_session(arena, "ada", ada, sizeof(ada));
    account_with_session(arena, "bob", bob, sizeof(bob));

    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port }), "while starting the files test server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(files), "while mounting the file routes");

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_ConstCString file_bytes = "the file bytes, zeroes and all";
    u64              file_id    = 0;

    // TEST: ada uploads a file and gets its id back
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = upload_request(arena, ada, "hello.txt", "text/plain", file_bytes);
        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_check(status_of(answer) == 201, "the upload is created, got '%.20s'", answer);

        const char* id = strstr(answer, "\"id\":");
        nya_check(id != nullptr, "the answer names the file id");
        if (id != nullptr) file_id = strtoull(id + 5, nullptr, 10);
        nya_check(file_id > 0, "the id is a real row id, got " FMTu64, file_id);
    }

    // TEST: ada downloads it back, bytes and content type intact, served as an attachment
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = nya_string_sprintf(
            arena,
            "GET /api/files?id=%llu HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n",
            (unsigned long long)file_id,
            ada
        );

        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_check(status_of(answer) == 200, "the owner downloads it, got '%.20s'", answer);
        nya_check(has_header(answer, "Content-Type", "text/plain"), "the content type round-trips");
        nya_check(
            has_header(answer, "Content-Disposition", "attachment; filename=\"hello.txt\""),
            "the filename comes back in an attachment disposition"
        );

        const char* body = body_of(answer);
        nya_check(body != nullptr && strncmp(body, file_bytes, strlen(file_bytes)) == 0, "the bytes survived the round trip");
    }

    // TEST: bob, handed ada's id, is refused — an id is not a capability
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = nya_string_sprintf(
            arena,
            "GET /api/files?id=%llu HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n",
            (unsigned long long)file_id,
            bob
        );

        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 403, "another account guessing the id is refused, got '%.20s'", answer);
        nya_check(strstr(body_of(answer), file_bytes) == nullptr, "and gets none of the bytes");
    }

    // TEST: a download with no session is nobody
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = nya_string_sprintf(
            arena,
            "GET /api/files?id=%llu HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n",
            (unsigned long long)file_id
        );

        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 401, "no cookie is nobody, got '%.20s'", answer);
    }

    // TEST: a missing, a non-numeric, and an unknown id are 400, 400 and 404
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* no_id = nya_string_sprintf(
            arena,
            "GET /api/files HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n",
            ada
        );
        nya_assert(exchange_text(client, nya_string_to_cstring(arena, no_id), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 400, "no id is a bad request, got '%.20s'", answer);

        NYA_String* bad_id = nya_string_sprintf(
            arena,
            "GET /api/files?id=notanumber HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n",
            ada
        );
        nya_assert(exchange_text(client, nya_string_to_cstring(arena, bad_id), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 400, "a non-numeric id is a bad request, got '%.20s'", answer);

        NYA_String* unknown = nya_string_sprintf(
            arena,
            "GET /api/files?id=999999 HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n",
            ada
        );
        nya_assert(exchange_text(client, nya_string_to_cstring(arena, unknown), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 404, "an unknown id is not found, got '%.20s'", answer);
    }

    // TEST: an upload with no file part is a bad request
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_ConstCString multipart = "--BOUNDARY\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\njust a field\r\n--BOUNDARY--\r\n";
        NYA_String*      request   = nya_string_sprintf(
            arena,
            "POST /api/files HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\n"
            "Content-Type: multipart/form-data; boundary=BOUNDARY\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
            ada,
            strlen(multipart),
            multipart
        );

        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 400, "an upload with no file is a bad request, got '%.20s'", answer);
    }

    // TEST: an upload larger than the route's limit is refused early with 413
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        // A file part past NYA_HTTP_FILES_MAX_UPLOAD_BYTES, but a whole body still under the server's own request ceiling, so the route's limit is
        // the one that answers.
        NYA_String* big = nya_string_create(arena);
        for (u64 index = 0; index < NYA_HTTP_FILES_MAX_UPLOAD_BYTES + 64; index++) nya_string_extend(big, "a");

        NYA_String* request = upload_request(arena, ada, "big.bin", "application/octet-stream", nya_string_to_cstring(arena, big));
        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 413, "an oversize upload is refused, got '%.20s'", answer);
    }

    // TEST: a non-multipart body on the upload route is unsupported media
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        const char* body    = "{\"not\":\"multipart\"}";
        NYA_String* request = nya_string_sprintf(
            arena,
            "POST /api/files HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
            ada,
            strlen(body),
            body
        );

        nya_assert(exchange_text(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 415, "a non-multipart upload is unsupported media, got '%.20s'", answer);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
