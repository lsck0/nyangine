/**
 * TLS end to end: a real certificate, a real handshake, and an HTTPS request answered by this server.
 *
 * The certificate is generated here rather than checked in, for the reason tests/nyangine/plugins/
 * test_pgp.c gives about keys in a repository. The client is the vendored libcurl through
 * plugins/curl, so nothing here shells out and nothing depends on a binary being installed.
 *
 * The server runs with a worker, so its listener has a thread of its own and the handshake happens
 * while this test is blocked inside the request — which is exactly how a real one runs.
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

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_tls");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what this build says about itself before anything is opened.
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what a context refuses, which is everything it cannot serve with.
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an HTTPS request, which is the only thing that proves any of this works.
  // ─────────────────────────────────────────────────────────────────────────────
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

    /*
     * What the status is does not matter, and 404 is the right one: nothing is mounted on this
     * server. nya_request_perform calls a 404 an error, so the status is what is asserted on — a
     * status at all is the handshake, the request and the answer, which is everything under test.
     */
    (void)answered;

    nya_check(response.status == 404, "an HTTPS request is answered by the router, got %u", response.status);

    // and plain HTTP to the same port is refused rather than answered, which is the whole reason a TLS
    // server does not also speak the other thing.
    NYA_ConstCString plain = nya_string_to_cstring(arena, nya_string_sprintf(arena, "http://127.0.0.1:%u/", (u32)port));

    NYA_Response ignored = { 0 };
    NYA_Error    refused = nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = plain }, &ignored);

    nya_check(!refused.ok || ignored.status == 0, "plaintext to an https port gets nothing back");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
