/**
 * The passwordless login end to end: a real port, a real socket, and a session opened by a passkey with no
 * password anywhere in it.
 *
 * test_accounts_passkey.c proves the WebAuthn primitives in the small — a credential enrolled, an assertion
 * verified, every way an assertion is not one. test_login_flow.c proves the password routes over a loopback
 * socket. This proves the seam between the passkey primitives and the mounted accounts router: that
 * the passkey register routes can create an account that never holds a password and log it straight in, that
 * the passkey login routes open the same `__Host-session` cookie the password login does, that the challenge
 * behind a login is single-use so a captured assertion cannot be replayed, and that a tampered assertion is
 * the same 401 as anything else.
 *
 * There is no browser or authenticator here, so the test is its own the way test_accounts_passkey.c is: it
 * generates an Ed25519 key with monocypher, hand-builds the exact clientDataJSON, authenticatorData and
 * attestationObject a real authenticator would, signs with the key it made, and hands them to the routes as
 * base64url over the wire. The server is driven by hand — a request written, nya_system_http_tick running the
 * handler on this thread, the answer read back — against an in-memory database, exactly as test_login_flow.c.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <string.h>
#include <time.h>

#define RP_ID  "example.com"
#define ORIGIN "https://example.com"

#define USERNAME "ada"

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * A TINY CBOR WRITER, ENOUGH TO BUILD WHAT AN AUTHENTICATOR SENDS
 * (the same one test_accounts_passkey.c uses; a relying party is handed CBOR either way)
 * ─────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    u8* bytes;
    u64 length;
    u64 capacity;
} Buffer;

static void put_byte(Buffer* buffer, u8 byte) {
    nya_assert(buffer->length < buffer->capacity);
    buffer->bytes[buffer->length++] = byte;
}

static void put_bytes(Buffer* buffer, const u8* data, u64 size) {
    for (u64 index = 0; index < size; index++) put_byte(buffer, data[index]);
}

static void cbor_head(Buffer* buffer, u8 major, u64 argument) {
    u8 tag = (u8)(major << 5);

    if (argument < 24) {
        put_byte(buffer, (u8)(tag | (u8)argument));
    } else if (argument <= 0xFF) {
        put_byte(buffer, (u8)(tag | 24));
        put_byte(buffer, (u8)argument);
    } else if (argument <= 0xFFFF) {
        put_byte(buffer, (u8)(tag | 25));
        put_byte(buffer, (u8)(argument >> 8));
        put_byte(buffer, (u8)argument);
    } else {
        put_byte(buffer, (u8)(tag | 26));
        put_byte(buffer, (u8)(argument >> 24));
        put_byte(buffer, (u8)(argument >> 16));
        put_byte(buffer, (u8)(argument >> 8));
        put_byte(buffer, (u8)argument);
    }
}

static void cbor_uint(Buffer* buffer, u64 value) { cbor_head(buffer, 0, value); }
static void cbor_nint(Buffer* buffer, u64 magnitude) { cbor_head(buffer, 1, magnitude - 1); }

static void cbor_bytes(Buffer* buffer, const u8* data, u64 size) {
    cbor_head(buffer, 2, size);
    put_bytes(buffer, data, size);
}

static void cbor_text(Buffer* buffer, const char* text) {
    u64 size = strlen(text);
    cbor_head(buffer, 3, size);
    put_bytes(buffer, (const u8*)text, size);
}

static void cbor_map(Buffer* buffer, u64 pairs) { cbor_head(buffer, 5, pairs); }

/** A COSE_Key map for an Ed25519 public key: kty OKP, alg EdDSA, crv Ed25519, x the 32 key bytes. */
static void put_cose_ed25519(Buffer* buffer, const u8 public_key[32]) {
    cbor_map(buffer, 4);
    cbor_uint(buffer, 1);
    cbor_uint(buffer, 1); // kty = OKP
    cbor_uint(buffer, 3);
    cbor_nint(buffer, 8); // alg = -8 (EdDSA)
    cbor_nint(buffer, 1);
    cbor_uint(buffer, 6); // crv (label -1) = Ed25519
    cbor_nint(buffer, 2);
    cbor_bytes(buffer, public_key, 32); // x (label -2)
}

/** The authenticator data: the RP id hash, a flags byte, a big-endian counter, and — for registration — the credential. */
static u64 build_authenticator_data(Buffer* out, u8 flags, u32 sign_count, const u8* credential_id, u64 credential_id_length, const Buffer* cose) {
    NYA_CryptoSha256Digest hash = { 0 };
    nya_crypto_sha256((const u8*)RP_ID, strlen(RP_ID), &hash);
    put_bytes(out, hash.bytes, 32);

    put_byte(out, flags);

    put_byte(out, (u8)(sign_count >> 24));
    put_byte(out, (u8)(sign_count >> 16));
    put_byte(out, (u8)(sign_count >> 8));
    put_byte(out, (u8)sign_count);

    if (cose != nullptr) {
        u8 aaguid[16] = { 0 };
        put_bytes(out, aaguid, sizeof(aaguid));

        put_byte(out, (u8)(credential_id_length >> 8));
        put_byte(out, (u8)credential_id_length);
        put_bytes(out, credential_id, credential_id_length);

        put_bytes(out, cose->bytes, cose->length);
    }

    return out->length;
}

/** An attestation object with the "none" format: fmt, the authenticator data, and an empty statement. */
static void build_attestation_object(Buffer* out, const Buffer* authenticator_data) {
    cbor_map(out, 3);
    cbor_text(out, "fmt");
    cbor_text(out, "none");
    cbor_text(out, "authData");
    cbor_bytes(out, authenticator_data->bytes, authenticator_data->length);
    cbor_text(out, "attStmt");
    cbor_map(out, 0);
}

/** A clientDataJSON. The challenge and origin have no JSON-special characters, so no escaping is needed. */
static u64 build_client_data(char* out, u64 capacity, const char* type, const char* challenge, const char* origin) {
    s32 written = snprintf(out, capacity, "{\"type\":\"%s\",\"challenge\":\"%s\",\"origin\":\"%s\"}", type, challenge, origin);
    nya_assert(written > 0 && (u64)written < capacity);
    return (u64)written;
}

/** Base64url of a byte buffer, into a caller buffer, for a JSON body field. */
static void b64url(NYA_Arena* arena, const u8* data, u64 size, OUT char** out_text) {
    u64   capacity = ((size + 2) / 3) * 4 + 1;
    char* text     = nya_arena_alloc(arena, capacity);
    u64   written  = 0;
    nya_check(nya_crypto_base64url_encode(data, size, text, capacity, &written), "the field encodes as base64url");
    *out_text = text;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * THE LOOPBACK HARNESS: a real port, a real socket, one whole answer at a time
 * (mirrors test_login_flow.c)
 * ─────────────────────────────────────────────────────────────────────────────
 */

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

static u32 status_of(const char* answer) {
    if (strncmp(answer, "HTTP/1.1 ", 9) != 0) return 0;
    return (u32)strtoul(answer + 9, nullptr, 10);
}

/** The value of a Set-Cookie line by cookie name, from after the `=` to the first `;`. False when there is none. */
static b8 cookie_value(const char* answer, const char* name, OUT char* out, u64 capacity) {
    out[0] = '\0';

    char needle[128] = { 0 };
    (void)snprintf(needle, sizeof(needle), "Set-Cookie: %s=", name);

    const char* set = strstr(answer, needle);
    if (set == nullptr) return false;

    const char* value = set + strlen(needle);
    const char* end   = strchr(value, ';');
    if (end == nullptr) return false;

    u64 length = (u64)(end - value);
    if (length == 0 || length + 1 > capacity) return false;

    nya_memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

/** The string value of a top-level JSON field in a response body, from after `"key":"` to the next `"`. */
static b8 json_string(const char* answer, const char* key, OUT char* out, u64 capacity) {
    out[0] = '\0';

    const char* body = strstr(answer, "\r\n\r\n");
    if (body == nullptr) return false;
    body += 4;

    char needle[64] = { 0 };
    (void)snprintf(needle, sizeof(needle), "\"%s\":\"", key);

    const char* at = strstr(body, needle);
    if (at == nullptr) return false;

    const char* value = at + strlen(needle);
    const char* end   = strchr(value, '"');
    if (end == nullptr) return false;

    u64 length = (u64)(end - value);
    if (length + 1 > capacity) return false;

    nya_memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

/** POSTs a JSON body with an optional Cookie header, and reads the whole answer back. */
static u64 post_json(NYA_Arena* arena, u16 port, const char* path, const char* cookie, const char* body, OUT char* answer, u64 capacity) {
    NYA_OsSocket client = connect_to(port);
    defer        nya_os_socket_close(client);

    NYA_String* request = cookie != nullptr && cookie[0] != '\0'
        ? nya_string_sprintf(arena,
                             "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nCookie: %s\r\n"
                             "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
                             path, cookie, strlen(body), body)
        : nya_string_sprintf(arena,
                             "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                             "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n%s",
                             path, strlen(body), body);

    return exchange(client, nya_string_to_cstring(arena, request), answer, capacity);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_passkey_login_flow");
    defer      nya_arena_destroy(arena);

    nya_account_throttle_reset();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    NYA_EXPECT(nya_accounts_open(arena, db));
    defer nya_accounts_close();
    defer nya_sql_close(db);

    // The key the person's "authenticator" holds. A fixed seed makes the test deterministic, as in the unit test.
    NYA_CryptoKey32 seed = { 0 };
    for (u32 index = 0; index < 32; index++) seed.bytes[index] = (u8)(index + 1);

    NYA_CryptoSignKeyPair key_pair = { 0 };
    nya_crypto_sign_key_pair_from_seed(&seed, &key_pair);

    // A credential id the authenticator minted, and its base64url form, which is what the routes speak.
    u8 credential_id[16] = { 0 };
    for (u32 index = 0; index < sizeof(credential_id); index++) credential_id[index] = (u8)(0xA0 + index);

    char* credential_id_b64 = nullptr;
    b64url(arena, credential_id, sizeof(credential_id), &credential_id_b64);

    Buffer cose = { .bytes = (u8[512]){ 0 }, .length = 0, .capacity = 512 };
    put_cose_ed25519(&cose, key_pair.public_key.bytes);

    // The seal key the accounts routes bind their pending cookies with; a fixed one is fine for a test.
    u8 seal[32] = { 0 };
    for (u32 index = 0; index < sizeof(seal); index++) seal[index] = (u8)(0x40 + index);

    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port }), "while starting the passkey test server");
    defer nya_system_http_deinit();

    const NYA_HttpRouter* accounts = nya_http_accounts_open((NYA_HttpAccountsConfig){
        .arena                  = arena,
        .database               = db,
        .registration           = NYA_ACCOUNT_REGISTRATION_OPEN,
        .totp_issuer            = "test",
        .login_seal_secret      = seal,
        .login_seal_secret_size = sizeof(seal),
        .passkey_rp_id          = RP_ID,
        .passkey_origin         = ORIGIN,
    });
    nya_assert(accounts != nullptr);
    defer nya_http_accounts_close();

    NYA_EXPECT(nya_http_server_merge(accounts), "while mounting the accounts routes");

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    char register_cookie[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    char login_cookie[NYA_HTTP_SEAL_MAX_TOKEN]    = { 0 };
    char session[NYA_ACCOUNTS_TOKEN_TEXT_BYTES]   = { 0 };
    char challenge[NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT] = { 0 };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a passwordless account is created and enrolled, and register/finish logs it straight in
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // begin, signed out: the account is created with no password, and a challenge and a register cookie come back.
        nya_assert(post_json(arena, port, "/api/passkey/register/begin", nullptr, "{\"username\":\"" USERNAME "\"}", answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200, "register/begin answers the create options, got '%.15s'", answer);
        nya_check(json_string(answer, "challenge", challenge, sizeof(challenge)), "and carries a challenge");
        nya_check(nya_string_contains(answer, "\"alg\":-8"), "and offers only Ed25519");
        nya_check(cookie_value(answer, "__Host-passkey-register", register_cookie, sizeof(register_cookie)), "and binds the enrolment in a sealed cookie");

        // The authenticator answers a create response for that challenge.
        Buffer authenticator_data = { .bytes = (u8[1024]){ 0 }, .length = 0, .capacity = 1024 };
        build_authenticator_data(&authenticator_data, 0x41 /* UP | AT */, 0, credential_id, sizeof(credential_id), &cose);
        Buffer attestation = { .bytes = (u8[1024]){ 0 }, .length = 0, .capacity = 1024 };
        build_attestation_object(&attestation, &authenticator_data);

        char client_data[512] = { 0 };
        u64  client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge, ORIGIN);

        char* client_data_b64 = nullptr;
        char* attestation_b64 = nullptr;
        b64url(arena, (const u8*)client_data, client_data_size, &client_data_b64);
        b64url(arena, attestation.bytes, attestation.length, &attestation_b64);

        NYA_String* body = nya_string_sprintf(arena, "{\"client_data_json\":\"%s\",\"attestation_object\":\"%s\",\"name\":\"a phone\"}", client_data_b64, attestation_b64);

        char cookie[NYA_HTTP_SEAL_MAX_TOKEN + 64] = { 0 };
        (void)snprintf(cookie, sizeof(cookie), "__Host-passkey-register=%s", register_cookie);

        nya_assert(post_json(arena, port, "/api/passkey/register/finish", cookie, nya_string_to_cstring(arena, body), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 204, "register/finish stores the credential, got '%.15s'", answer);
        nya_check(cookie_value(answer, NYA_HTTP_SESSION_COOKIE, session, sizeof(session)), "and logs the new account in with a session cookie");
        nya_check(nya_string_contains(answer, "HttpOnly") && nya_string_contains(answer, "Secure") && nya_string_contains(answer, "SameSite=Strict"),
                  "the session cookie carries the flags a session cookie must");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the session the passkey sign-up opened names the account, and it has no password
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);

        NYA_String* request = nya_string_sprintf(arena, "GET /api/session HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n", session);
        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200, "the passkey session is recognised, got '%.15s'", answer);
        nya_check(nya_string_contains(answer, "\"username\":\"" USERNAME "\""), "and names the account it opened");

        // A password login for that account is refused: it never had one, which is the whole point.
        nya_account_throttle_reset();
        NYA_AccountUser user = { 0 };
        nya_check(!nya_account_authenticate(arena, USERNAME, "any password at all here", nullptr, &user).ok, "the account has no password to log in with");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a passwordless login opens a fresh session with the passkey and no password
    // ─────────────────────────────────────────────────────────────────────────────
    char assertion_body[2048] = { 0 };
    {
        // begin: the challenge and the account's credential, plus the login cookie the finish reads.
        nya_assert(post_json(arena, port, "/api/passkey/login/begin", nullptr, "{\"username\":\"" USERNAME "\"}", answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200, "login/begin answers the request options, got '%.15s'", answer);
        nya_check(json_string(answer, "challenge", challenge, sizeof(challenge)), "and carries an assertion challenge");
        nya_check(nya_string_contains(answer, credential_id_b64), "and lists the account's credential");
        nya_check(cookie_value(answer, "__Host-passkey-login", login_cookie, sizeof(login_cookie)), "and names the account in a sealed cookie");

        // The authenticator signs authenticatorData || SHA-256(clientDataJSON) with its key.
        Buffer authenticator_data = { .bytes = (u8[256]){ 0 }, .length = 0, .capacity = 256 };
        build_authenticator_data(&authenticator_data, 0x01 /* UP */, 1, nullptr, 0, nullptr);

        char client_data[512] = { 0 };
        u64  client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge, ORIGIN);

        NYA_CryptoSha256Digest client_hash = { 0 };
        nya_crypto_sha256((const u8*)client_data, client_data_size, &client_hash);

        u8 message[512] = { 0 };
        nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
        nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);

        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

        char* client_data_b64        = nullptr;
        char* authenticator_data_b64 = nullptr;
        char* signature_b64          = nullptr;
        b64url(arena, (const u8*)client_data, client_data_size, &client_data_b64);
        b64url(arena, authenticator_data.bytes, authenticator_data.length, &authenticator_data_b64);
        b64url(arena, signature.bytes, sizeof(signature.bytes), &signature_b64);

        (void)snprintf(assertion_body, sizeof(assertion_body),
                       "{\"credential_id\":\"%s\",\"client_data_json\":\"%s\",\"authenticator_data\":\"%s\",\"signature\":\"%s\"}",
                       credential_id_b64, client_data_b64, authenticator_data_b64, signature_b64);

        char cookie[NYA_HTTP_SEAL_MAX_TOKEN + 64] = { 0 };
        (void)snprintf(cookie, sizeof(cookie), "__Host-passkey-login=%s", login_cookie);

        char logged_in[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
        nya_assert(post_json(arena, port, "/api/passkey/login/finish", cookie, assertion_body, answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 204, "login/finish verifies the assertion and sets a session, got '%.15s'", answer);
        nya_check(cookie_value(answer, NYA_HTTP_SESSION_COOKIE, logged_in, sizeof(logged_in)), "with a __Host-session cookie");
        nya_check(logged_in[0] != '\0' && strcmp(logged_in, session) != 0, "a fresh session, not the sign-up's");

        // The fresh cookie is who you are on the next request.
        NYA_OsSocket client = connect_to(port);
        defer        nya_os_socket_close(client);
        NYA_String* request = nya_string_sprintf(arena, "GET /api/session HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " NYA_HTTP_SESSION_COOKIE "=%s\r\nConnection: keep-alive\r\n\r\n", logged_in);
        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200 && nya_string_contains(answer, "\"username\":\"" USERNAME "\""), "and the passwordless session names the account");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the same assertion cannot be replayed — the challenge behind it was single-use
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // Re-begin to get a fresh login cookie, then submit the OLD assertion, whose challenge is spent.
        nya_assert(post_json(arena, port, "/api/passkey/login/begin", nullptr, "{\"username\":\"" USERNAME "\"}", answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200, "a second login/begin answers, got '%.15s'", answer);
        nya_check(cookie_value(answer, "__Host-passkey-login", login_cookie, sizeof(login_cookie)), "with its own login cookie");

        char cookie[NYA_HTTP_SEAL_MAX_TOKEN + 64] = { 0 };
        (void)snprintf(cookie, sizeof(cookie), "__Host-passkey-login=%s", login_cookie);

        nya_assert(post_json(arena, port, "/api/passkey/login/finish", cookie, assertion_body, answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 401, "a replayed assertion is refused, got '%.15s'", answer);
        nya_check(!nya_string_contains(answer, "Set-Cookie: " NYA_HTTP_SESSION_COOKIE), "and no session is handed out for it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a tampered assertion is the same 401, and an unknown username is told nothing apart
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // A fresh, valid challenge, but a signature with one flipped byte.
        nya_assert(post_json(arena, port, "/api/passkey/login/begin", nullptr, "{\"username\":\"" USERNAME "\"}", answer, sizeof(answer)) > 0);
        nya_check(json_string(answer, "challenge", challenge, sizeof(challenge)), "a fresh challenge");
        nya_check(cookie_value(answer, "__Host-passkey-login", login_cookie, sizeof(login_cookie)), "and login cookie");

        Buffer authenticator_data = { .bytes = (u8[256]){ 0 }, .length = 0, .capacity = 256 };
        build_authenticator_data(&authenticator_data, 0x01, 2, nullptr, 0, nullptr);
        char client_data[512] = { 0 };
        u64  client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge, ORIGIN);
        NYA_CryptoSha256Digest client_hash = { 0 };
        nya_crypto_sha256((const u8*)client_data, client_data_size, &client_hash);
        u8 message[512] = { 0 };
        nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
        nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);
        signature.bytes[0] ^= 0xFF;

        char* client_data_b64        = nullptr;
        char* authenticator_data_b64 = nullptr;
        char* signature_b64          = nullptr;
        b64url(arena, (const u8*)client_data, client_data_size, &client_data_b64);
        b64url(arena, authenticator_data.bytes, authenticator_data.length, &authenticator_data_b64);
        b64url(arena, signature.bytes, sizeof(signature.bytes), &signature_b64);

        NYA_String* body = nya_string_sprintf(arena, "{\"credential_id\":\"%s\",\"client_data_json\":\"%s\",\"authenticator_data\":\"%s\",\"signature\":\"%s\"}",
                                              credential_id_b64, client_data_b64, authenticator_data_b64, signature_b64);
        char cookie[NYA_HTTP_SEAL_MAX_TOKEN + 64] = { 0 };
        (void)snprintf(cookie, sizeof(cookie), "__Host-passkey-login=%s", login_cookie);
        nya_assert(post_json(arena, port, "/api/passkey/login/finish", cookie, nya_string_to_cstring(arena, body), answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 401, "a tampered assertion is refused, got '%.15s'", answer);

        // An unknown username gets a 200 with a decoy challenge and no credential — the same shape a real one gets.
        char decoy[NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT] = { 0 };
        nya_assert(post_json(arena, port, "/api/passkey/login/begin", nullptr, "{\"username\":\"nobody-here\"}", answer, sizeof(answer)) > 0);
        nya_check(status_of(answer) == 200, "an unknown username still answers 200, got '%.15s'", answer);
        nya_check(json_string(answer, "challenge", decoy, sizeof(decoy)) && decoy[0] != '\0', "with a decoy challenge");
        nya_check(!nya_string_contains(answer, credential_id_b64), "and nobody's credentials");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
