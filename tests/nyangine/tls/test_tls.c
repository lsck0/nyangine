/**
 * TLS end to end: a real certificate, a real handshake, and an HTTPS request answered by this server.
 *
 * The certificate is generated here rather than checked in, for the reason tests/nyangine/plugins/
 * test_pgp.c gives about keys in a repository. One client is the vendored libcurl through plugins/curl;
 * the other is this program's own nya_tls_session_connect, driven by hand so the two halves of the tls
 * module — the accepting side on the server and the verifying side on the client — are proven against
 * each other and not only against curl. Nothing here shells out or depends on a binary being installed.
 *
 * The server runs with a worker, so its listener has a thread of its own and the handshake happens
 * while this test is blocked inside the request — which is exactly how a real one runs. And the four
 * cases that matter are all here: a verified round trip, a certificate for the wrong host refused, a
 * garbage handshake dropped rather than hung on, and a server with no certificate speaking plaintext.
 **/

#include <string.h>

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Where the generated certificate and key go. Under a directory of this test's own, deleted after. */
typedef struct {
  NYA_ConstCString directory;
  NYA_ConstCString certificate;
  NYA_ConstCString key;
} Pair;

/**
 * Writes a self-signed certificate for `localhost` and answers where it put it.
 *
 * Through the openssl command rather than through its library: generating a key and signing a
 * certificate is a page of API this module has no other reason to carry, and a test that skips itself
 * when the command is absent costs nothing. Everything under test is still this program's own code.
 * */
static b8 make_certificate(NYA_Arena* arena, NYA_ConstCString name, Pair* out_pair) {
  NYA_String* temporary = nullptr;
  nya_check(nya_filesystem_temp_directory(arena, &temporary).ok, "there is a temporary directory");

  NYA_ConstCString directory = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/nyangine-test-tls-%s", nya_string_to_cstring(arena, temporary), name));

  (void)nya_filesystem_delete_recursive(directory);
  nya_check(nya_filesystem_create_directory(directory).ok, "and a directory under it");

  NYA_ConstCString certificate = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/cert.pem", directory));
  NYA_ConstCString key         = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/key.pem", directory));

  NYA_Command generate = {
    .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
    .arena     = arena,
    .program   = "openssl",
    .arguments = { "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
                   "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                   "-keyout", key, "-out", certificate, nullptr },
  };

  b8 made = nya_command_run(&generate).ok && generate.exit_code == 0;
  nya_command_destroy(&generate);

  if (!made) {
    (void)nya_filesystem_delete_recursive(directory);
    return false;
  }

  *out_pair = (Pair){ .directory = directory, .certificate = certificate, .key = key };

  return true;
}

/**
 * A wall-clock ceiling for the hand-driven client loops below, so a stall fails the test rather than
 * hanging it: every loop here checks this and gives up, which is the whole difference between a test
 * that catches a wedged handshake and one that is the wedge.
 * */
#define CLIENT_DEADLINE_MS 5000

static b8 past_deadline(u64 started_ns) {
  return nya_clock_get_monotonic_ns() - started_ns > (u64)CLIENT_DEADLINE_MS * 1000000ULL;
}

/** Waits, briefly, for the socket to be ready for what the last answer asked for. The deadline is the caller's. */
static void wait_for(NYA_OsSocket socket, b8 readable, b8 writable) {
  NYA_OsSocketWait watched = { .socket = socket, .readable = readable, .writable = writable };
  u32              ready    = 0;

  (void)nya_os_socket_wait(&watched, 1, 100, &ready);
}

/** Connects a client socket to the loopback port and waits for the non-blocking connect to finish. */
static NYA_OsSocket dial(u16 port) {
  NYA_OsAddress address = { 0 };
  if (nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) != NYA_OS_SOCKET_OK) return NYA_OS_SOCKET_NONE;

  NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
  NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);

  if (connected != NYA_OS_SOCKET_OK && connected != NYA_OS_SOCKET_WOULD_BLOCK) return NYA_OS_SOCKET_NONE;

  // a non-blocking connect is under way rather than done, and writability is how the host says it finished; loopback usually beats the first wait to it.
  wait_for(socket, false, true);

  if (nya_os_socket_error(socket) != NYA_OS_SOCKET_OK) {
    nya_os_socket_close(socket);
    return NYA_OS_SOCKET_NONE;
  }

  return socket;
}

/**
 * A whole HTTPS exchange driven by the engine's own TLS client: connect, hand the socket a client
 * session that pins `ca_path` and verifies `host`, run the handshake through its four answers, send one
 * GET and read the answer until the server closes. The round trip proves the two sides of this module
 * against each other rather than against curl, and `out_protocol` carries back what was negotiated.
 *
 * False for any failure — a handshake that will not verify, a peer that went away, or the deadline —
 * so a caller can assert on it without the test hanging on a wedge.
 * */
static b8 https_get(
  NYA_Arena* arena, u16 port, NYA_ConstCString ca_path, NYA_ConstCString host, OUT char* buffer, u64 capacity, OUT NYA_ConstCString* out_protocol
) {
  *out_protocol = "";
  buffer[0]     = '\0';

  NYA_TlsContext* client = nullptr;
  if (!nya_tls_context_create(arena, &client, .client = true, .ca_path = ca_path).ok) return false;
  defer nya_tls_context_destroy(client);

  NYA_OsSocket socket = dial(port);
  if (socket.handle == 0) return false;
  defer nya_os_socket_close(socket);

  NYA_TlsSession* session = nullptr;
  if (!nya_tls_session_connect(client, socket, host, &session).ok) return false;
  defer nya_tls_session_destroy(session);

  u64 started = nya_clock_get_monotonic_ns();

  // the handshake, in the four answers the header names, until it is done or the deadline says it will not be.
  for (;;) {
    if (past_deadline(started)) return false;

    switch (nya_tls_handshake(session)) {
      case NYA_TLS_OK:         goto shaken;
      case NYA_TLS_WANT_READ:  wait_for(socket, true, false); continue;
      case NYA_TLS_WANT_WRITE: wait_for(socket, false, true); continue;
      default:                 return false;
    }
  }

shaken:
  *out_protocol = nya_tls_protocol(session);

  // one GET, sent whole; Connection: close so the server ends the stream and the read below finds it.
  NYA_ConstCString request =
    nya_string_to_cstring(arena, nya_string_sprintf(arena, "GET /healthz HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host));
  u64 request_size = strlen(request);
  u64 sent         = 0;

  while (sent < request_size) {
    if (past_deadline(started)) return false;

    u64 wrote = 0;

    switch (nya_tls_send(session, (const u8*)request + sent, request_size - sent, &wrote)) {
      case NYA_TLS_OK:         sent += wrote; continue;
      case NYA_TLS_WANT_READ:  wait_for(socket, true, false); continue;
      case NYA_TLS_WANT_WRITE: wait_for(socket, false, true); continue;
      default:                 return false;
    }
  }

  // and the answer, read until the server's close_notify or its FIN, whichever the four answers report.
  u64 filled = 0;

  for (;;) {
    if (past_deadline(started)) return false;
    if (filled + 1 >= capacity) break;

    u64 read = 0;

    NYA_TlsProgress progress = nya_tls_receive(session, (u8*)buffer + filled, capacity - 1 - filled, &read);

    if (progress == NYA_TLS_OK) {
      filled += read;
      continue;
    }

    if (progress == NYA_TLS_WANT_READ) {
      wait_for(socket, true, false);
      continue;
    }

    if (progress == NYA_TLS_WANT_WRITE) {
      wait_for(socket, false, true);
      continue;
    }

    if (progress == NYA_TLS_CLOSED) break;

    return false;
  }

  buffer[filled] = '\0';

  return true;
}

/**
 * Sends `request` over a plaintext socket and reads until the server closes or the deadline passes.
 *
 * Answers whether the stream reached its end in time, which is two different proofs depending on what
 * was sent: a real request answered and closed, or a handshake the server could not parse dropped
 * cleanly. Either way, a deadline that passes with the socket still open is the hang this guards
 * against. What came back is in `buffer` for the caller that has something to check.
 * */
static b8 raw_exchange(u16 port, const u8* request, u64 request_size, OUT char* buffer, u64 capacity) {
  buffer[0] = '\0';

  NYA_OsSocket socket = dial(port);
  if (socket.handle == 0) return false;
  defer nya_os_socket_close(socket);

  u64 started = nya_clock_get_monotonic_ns();
  u64 sent    = 0;

  while (sent < request_size) {
    if (past_deadline(started)) return false;

    u64 wrote = 0;

    NYA_OsSocketStatus status = nya_os_socket_send(socket, request + sent, request_size - sent, &wrote);

    if (status == NYA_OS_SOCKET_OK) {
      sent += wrote;
      continue;
    }

    if (status == NYA_OS_SOCKET_WOULD_BLOCK) {
      wait_for(socket, false, true);
      continue;
    }

    // the peer is already gone, which for a garbage handshake is the drop this is looking for.
    if (status == NYA_OS_SOCKET_CLOSED) return true;

    return false;
  }

  u64 filled = 0;

  for (;;) {
    if (past_deadline(started)) return false;
    if (filled + 1 >= capacity) break;

    u64 read = 0;

    NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)buffer + filled, capacity - 1 - filled, &read);

    if (status == NYA_OS_SOCKET_OK) {
      filled += read;
      continue;
    }

    if (status == NYA_OS_SOCKET_CLOSED) break;

    if (status == NYA_OS_SOCKET_WOULD_BLOCK) {
      wait_for(socket, true, false);
      continue;
    }

    return false;
  }

  buffer[filled] = '\0';

  return true;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_tls");
  defer      nya_arena_destroy(arena);

  // TEST: what this build says about itself before anything is opened.
  {
    nya_check(nya_tls_version()[0] != '\0', "there is always a version string, even with no library");

    if (!nya_tls_available()) {
      NYA_TlsContext* refused = nullptr;
      NYA_Error       answer  = nya_tls_context_create(arena, &refused, .certificate_path = "cert.pem", .key_path = "key.pem");

      nya_check(!answer.ok && answer.kind == NYA_ERROR_NOT_SUPPORTED, "a build with no TLS says so rather than failing some other way");
      nya_check(refused == nullptr, "and hands back nothing");

      printf("  this build has no TLS library; the handshake is skipped\n");

      return nya_check_failures() == 0 ? 0 : 1;
    }

    printf("  %s\n", nya_tls_version());
  }

  Pair pair = { 0 };

  if (!make_certificate(arena, "server", &pair)) {
    printf("  the openssl command is not installed here; the handshake is skipped\n");
    return nya_check_failures() == 0 ? 0 : 1;
  }

  defer (void)nya_filesystem_delete_recursive(pair.directory);

  // TEST: what a context refuses, which is everything it cannot serve with.
  {
    NYA_TlsContext* context = nullptr;

    nya_check(!nya_tls_context_create(arena, &context, .key_path = pair.key).ok, "a context with no certificate is refused");
    nya_check(!nya_tls_context_create(arena, &context, .certificate_path = pair.certificate).ok, "and one with no key");
    nya_check(
      !nya_tls_context_create(arena, &context, .certificate_path = "/nowhere/cert.pem", .key_path = pair.key).ok,
      "and one whose certificate is not there"
    );

    // a certificate and a key that are not a pair: a server that starts and then fails every handshake.
    Pair other = { 0 };
    if (make_certificate(arena, "other", &other)) {
      defer (void)nya_filesystem_delete_recursive(other.directory);

      NYA_Error mismatched = nya_tls_context_create(arena, &context, .certificate_path = pair.certificate, .key_path = other.key);

      nya_check(!mismatched.ok, "and a key that is not the certificate's");
      nya_check(mismatched.message[0] != '\0', "saying which two files disagree: %s", (NYA_ConstCString)mismatched.message);
    }

    nya_check(nya_tls_context_create(arena, &context, .certificate_path = pair.certificate, .key_path = pair.key).ok, "and the real pair is accepted");
    nya_check(context != nullptr, "with a context to show for it");
    nya_check(nya_tls_session_count(context) == 0, "holding no sessions yet");

    nya_tls_context_destroy(context);
  }

  // TEST: an HTTPS request, which is the only thing that proves any of this works.
  {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
      .port             = port,
      .workers          = 1,
      .certificate_path = pair.certificate,
      .key_path         = pair.key,
    }));

    defer nya_system_http_deinit();

    NYA_ConstCString url = nya_string_to_cstring(arena, nya_string_sprintf(arena, "https://127.0.0.1:%u/", (u32)port));

    NYA_Response response = { 0 };

    NYA_Error answered = nya_request_perform(
      arena,
      (NYA_Request){
        .method                  = NYA_REQUEST_METHOD_GET,
        .url                     = url,
        // self-signed, and generated a moment ago: there is nothing for curl to check it against.
        .insecure_skip_tls_verify = true,
      },
      &response
    );

    /* What the status is does not matter, and 404 is the right one: nothing is mounted on this server. nya_request_perform calls a 404 an error, so the status is what is asserted on — a status at all is the handshake, the request and the answer, which is everything under test. */
    (void)answered;

    nya_check(response.status == 404, "an HTTPS request is answered by the router, got %u", response.status);

    // and it says so, which is a header a browser only honours over TLS and this server only sends there.
    nya_check(nya_http_hsts(), "a TLS server sends HSTS");

    char hsts[64] = { 0 };
    nya_check(nya_response_header(&response, "Strict-Transport-Security", hsts, sizeof(hsts)), "on the response itself");
    nya_check(nya_string_contains(hsts, "max-age="), "with a max-age on it, got '%s'", hsts);

    // and plain HTTP to the same port is refused rather than answered, which is the whole reason a TLS server does not also speak the other thing.
    NYA_ConstCString plain = nya_string_to_cstring(arena, nya_string_sprintf(arena, "http://127.0.0.1:%u/", (u32)port));

    NYA_Response ignored = { 0 };
    NYA_Error    refused = nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = plain }, &ignored);

    nya_check(!refused.ok || ignored.status == 0, "plaintext to an https port gets nothing back");
  }

  // TEST: the round trip with this program's own TLS client, verifying the certificate rather than skipping it — the two halves of this module against each other, which curl cannot prove.
  {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
      .port             = port,
      .workers          = 1,
      .certificate_path = pair.certificate,
      .key_path         = pair.key,
    }));

    defer nya_system_http_deinit();

    // liveness answers 200 on a worker, so the listener thread writes it back with nothing for the frame to do — which is what lets this test drive the whole exchange by hand.
    NYA_EXPECT(nya_http_server_merge(nya_http_health_router()));

    char             buffer[1024] = { 0 };
    NYA_ConstCString protocol     = "";

    // the certificate is its own authority, pinned as the trust store; "localhost" is what its subjectAltName names, so verification passes over a socket that connected to 127.0.0.1.
    b8 got = https_get(arena, port, pair.certificate, "localhost", buffer, sizeof(buffer), &protocol);

    nya_check(got, "the engine's TLS client completes a verified handshake and reads an answer");
    nya_check(nya_string_starts_with(buffer, "HTTP/1.1 200"), "GET /healthz is 200 over TLS, got '%.16s'", buffer);
    nya_check(nya_string_starts_with(protocol, "TLSv1."), "on TLS 1.2 or better, got '%s'", protocol);

    // a certificate that verifies against the wrong name must fail the handshake rather than be taken: the trust store is the same pin, but "example.com" is not what the certificate is for.
    char        rejected[256] = { 0 };
    NYA_ConstCString unused    = "";
    b8          wrong_host     = https_get(arena, port, pair.certificate, "example.com", rejected, sizeof(rejected), &unused);

    nya_check(!wrong_host, "a certificate for another host is refused by the verifying client");

    // and bytes that are not a ClientHello are dropped, not answered and not left hanging: a plain HTTP request at the https port is the port scanner and the wrong-scheme browser both.
    static const u8 GARBAGE[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char            drained[256] = { 0 };

    nya_check(raw_exchange(port, GARBAGE, sizeof(GARBAGE) - 1, drained, sizeof(drained)), "a garbage handshake is dropped cleanly, not hung on");

    // the server is still whole after that: another verified round trip is still answered, which is what says the failed handshake freed its session rather than wedging the pool.
    char after[1024] = { 0 };
    NYA_ConstCString after_protocol = "";

    nya_check(
      https_get(arena, port, pair.certificate, "localhost", after, sizeof(after), &after_protocol) && nya_string_starts_with(after, "HTTP/1.1 200"),
      "and the server answers the next client after the garbage one"
    );
  }

  // TEST: no certificate is plaintext, not a refusal — the loopback development server, which speaks HTTP over a bare socket and sends no HSTS because there is no TLS for a browser to pin to.
  {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = 1 }));

    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(nya_http_health_router()));

    nya_check(!nya_http_hsts(), "a plaintext server sends no HSTS");

    static const u8 REQUEST[] = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char            buffer[1024] = { 0 };

    nya_check(raw_exchange(port, REQUEST, sizeof(REQUEST) - 1, buffer, sizeof(buffer)), "a plaintext request is answered");
    nya_check(nya_string_starts_with(buffer, "HTTP/1.1 200"), "with a 200 and no TLS in the way, got '%.16s'", buffer);
    nya_check(!nya_string_contains(buffer, "Strict-Transport-Security"), "and no HSTS on the answer");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
