/**
 * The SMTP plugin, with no mail server.
 *
 * The message builder is a pure function, so its RFC 5322 output (the headers, the MIME framing, the
 * base64 bodies) is asserted directly, and a header value carrying a newline is shown to be refused
 * rather than turned into a header of its own. The conversation is curl's; what is this module's is the
 * no-plaintext rule, proven against a fake server on loopback that never offers STARTTLS: the send has
 * to fail, and the server must never hear a login or an envelope.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* A PLAINTEXT FAKE SERVER ON LOOPBACK */

/** How long the fake server waits on any one step before it gives up on the client. */
#define FAKE_STEP_TIMEOUT_MS 5000

typedef struct {
  NYA_OsSocket listener;

  /** Everything the client said, for the assertions after the join. */
  char heard[4096];
  u64  heard_size;
} FakeServer;

static b8 fake_wait(NYA_OsSocket socket) {
  NYA_OsSocketWait wait  = { .socket = socket, .readable = true };
  u32              ready = 0;
  return nya_os_socket_wait(&wait, 1, FAKE_STEP_TIMEOUT_MS, &ready) == NYA_OS_SOCKET_OK && ready > 0;
}

static void fake_say(NYA_OsSocket socket, NYA_ConstCString line) {
  u64 sent = 0;
  nya_assert(nya_os_socket_send(socket, (const u8*)line, strlen(line), &sent) == NYA_OS_SOCKET_OK, "the fake server could not write");
}

/** Reads until the peer goes quiet or closes; true when anything arrived. */
static b8 fake_listen(FakeServer* server, NYA_OsSocket socket) {
  if (!fake_wait(socket)) return false;

  u64                room   = sizeof(server->heard) - 1 - server->heard_size;
  u64                read   = 0;
  NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)server->heard + server->heard_size, room, &read);
  server->heard_size += read;

  return status == NYA_OS_SOCKET_OK && read > 0;
}

/** Greets, answers EHLO without STARTTLS, then records whatever the client still says until it hangs up. */
static void fake_server_run(void* data) {
  FakeServer* server = data;

  NYA_OsSocket client = NYA_OS_SOCKET_NONE;
  NYA_OsAddress from  = { 0 };
  if (!fake_wait(server->listener)) return;
  if (nya_os_socket_accept(server->listener, &client, &from) != NYA_OS_SOCKET_OK) return;
  defer nya_os_socket_close(client);

  fake_say(client, "220 fake.test ESMTP\r\n");
  if (!fake_listen(server, client)) return;

  fake_say(client, "250-fake.test\r\n250 AUTH PLAIN LOGIN\r\n");

  // a QUIT is all a refusing client should still send; answered, so it hangs up rather than waits.
  while (fake_listen(server, client)) {
    if (strstr(server->heard, "QUIT") != nullptr) fake_say(client, "221 bye\r\n");
  }
}

static NYA_ConstCString base64_of(NYA_Arena* arena, NYA_ConstCString text) {
  NYA_String* encoded = nya_string_create(arena);
  nya_base64_encode(encoded, (const u8*)text, strlen(text));
  return nya_string_to_cstring(arena, encoded);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_smtp");
  defer      nya_arena_destroy(arena);

  // TEST: the message builder produces a well-formed RFC 5322 blob.
  {
    NYA_SmtpMessage message = {
      .from      = "noreply@example.com",
      .from_name = "Example",
      .to        = "person@elsewhere.test",
      .subject   = "Confirm your address",
      .text      = "Open the link to confirm.",
    };

    NYA_String* blob = nullptr;
    nya_check(nya_smtp_message_build(arena, &message, &blob).ok, "a valid message builds");

    NYA_ConstCString text = nya_string_to_cstring(arena, blob);

    nya_check(nya_string_contains(text, "From: \"Example\" <noreply@example.com>\r\n"), "the From header carries the display name and the address");
    nya_check(nya_string_contains(text, "To: person@elsewhere.test\r\n"), "the To header is the recipient");
    nya_check(nya_string_contains(text, "Subject: Confirm your address\r\n"), "the Subject header is the subject");
    nya_check(nya_string_contains(text, "\r\nDate: "), "there is a Date header");
    nya_check(nya_string_contains(text, "\r\nMessage-ID: <"), "there is a Message-ID header");
    nya_check(nya_string_contains(text, "@example.com>\r\n"), "the Message-ID is at the sender's domain");
    nya_check(nya_string_contains(text, "MIME-Version: 1.0\r\n"), "there is a MIME-Version header");
    nya_check(nya_string_contains(text, "Content-Type: text/plain; charset=UTF-8\r\n"), "a text-only message is a single text/plain part");
    nya_check(nya_string_contains(text, "Content-Transfer-Encoding: base64\r\n"), "the body is base64 encoded");
    nya_check(nya_string_contains(text, base64_of(arena, "Open the link to confirm.")), "and the base64 body is exactly the text encoded");

    // The header block ends with a blank line before the body, which is what separates the two.
    nya_check(nya_string_contains(text, "\r\n\r\n"), "a blank line separates the headers from the body");
  }

  // TEST: an HTML alternative becomes a multipart/alternative with both bodies.
  {
    NYA_SmtpMessage message = {
      .from    = "noreply@example.com",
      .to      = "person@elsewhere.test",
      .subject = "Hello",
      .text    = "plain body",
      .html    = "<p>rich body</p>",
    };

    NYA_String* blob = nullptr;
    nya_check(nya_smtp_message_build(arena, &message, &blob).ok, "a message with HTML builds");

    NYA_ConstCString text = nya_string_to_cstring(arena, blob);

    nya_check(nya_string_contains(text, "Content-Type: multipart/alternative; boundary=\"=_nyangine_"), "it is a multipart/alternative");
    nya_check(nya_string_contains(text, "Content-Type: text/plain; charset=UTF-8\r\n"), "with a text/plain part");
    nya_check(nya_string_contains(text, "Content-Type: text/html; charset=UTF-8\r\n"), "and a text/html part");
    nya_check(nya_string_contains(text, base64_of(arena, "plain body")), "the text body base64 is present");
    nya_check(nya_string_contains(text, base64_of(arena, "<p>rich body</p>")), "the html body base64 is present");
    nya_check(nya_string_contains(text, "--\r\n"), "the multipart is closed with a final boundary");
  }

  // TEST: a CR or LF in a header value is refused, so no header can be injected.
  {
    NYA_String* blob = nullptr;

    NYA_SmtpMessage injected_subject = {
      .from = "a@b.test", .to = "c@d.test", .text = "body", .subject = "Hi\r\nBcc: victim@evil.test",
    };
    NYA_Error subject = nya_smtp_message_build(arena, &injected_subject, &blob);
    nya_check(!subject.ok && subject.kind == NYA_ERROR_INVALID_ARGUMENT, "a subject with a newline is refused");
    nya_check(blob == nullptr, "and nothing is built");

    NYA_SmtpMessage injected_to = {
      .from = "a@b.test", .to = "c@d.test\r\nRCPT TO:<victim@evil.test>", .text = "body", .subject = "Hi",
    };
    nya_check(nya_smtp_message_build(arena, &injected_to, &blob).kind == NYA_ERROR_INVALID_ARGUMENT, "a recipient with a newline is refused");

    NYA_SmtpMessage injected_name = {
      .from = "a@b.test", .from_name = "Ada\r\nX: y", .to = "c@d.test", .text = "body", .subject = "Hi",
    };
    nya_check(nya_smtp_message_build(arena, &injected_name, &blob).kind == NYA_ERROR_INVALID_ARGUMENT, "a display name with a newline is refused");
  }

  // TEST: a server that does not offer STARTTLS is refused, and never hears a login or an envelope.
  {
    nya_assert(nya_os_socket_start() == NYA_OS_SOCKET_OK);
    defer nya_os_socket_stop();

    NYA_OsAddress loopback = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", 0, NYA_OS_ADDRESS_NONE, &loopback) == NYA_OS_SOCKET_OK);

    FakeServer* server = nya_arena_alloc(arena, sizeof(FakeServer));
    nya_memset(server, 0, sizeof(*server));
    nya_assert(nya_os_socket_open_at(NYA_OS_SOCKET_LISTENER, loopback, 0, &server->listener) == NYA_OS_SOCKET_OK);
    defer nya_os_socket_close(server->listener);

    NYA_OsAddress bound = { 0 };
    nya_assert(nya_os_socket_address(server->listener, &bound) == NYA_OS_SOCKET_OK);

    NYA_Thread* thread = nullptr;
    nya_assert(nya_thread_spawn(arena, fake_server_run, server, "fake_smtp", &thread).ok);

    NYA_SmtpConfig config = {
      .host = "127.0.0.1", .port = bound.port, .security = NYA_SMTP_SECURITY_STARTTLS,
      .auth = NYA_SMTP_AUTH_PLAIN, .username = "user", .password = "hunter2", .timeout_ms = FAKE_STEP_TIMEOUT_MS,
    };
    NYA_SmtpMessage message = { .from = "noreply@example.com", .to = "person@elsewhere.test", .subject = "Hi", .text = "body" };

    NYA_Error refused = nya_smtp_send(arena, &config, &message);
    nya_thread_join(thread);

    nya_check(!refused.ok, "a server with no STARTTLS is refused");
    nya_check(refused.kind == NYA_ERROR_NOT_SUPPORTED, "as the missing upgrade, got %s", (NYA_ConstCString)refused.message);
    nya_check(strstr((NYA_ConstCString)refused.message, "hunter2") == nullptr, "and the error does not carry the password");

    nya_check(strstr(server->heard, "EHLO example.com") != nullptr, "the client introduced itself by the sender's domain, heard '%s'", server->heard);
    nya_check(strstr(server->heard, "AUTH") == nullptr, "but never tried to log in over plaintext");
    nya_check(strstr(server->heard, "MAIL FROM") == nullptr, "nor sent the envelope");
  }

  // TEST: a login asked for without credentials is the caller's mistake, refused before any connection.
  {
    NYA_SmtpConfig config = { .host = "smtp.test", .port = 587, .auth = NYA_SMTP_AUTH_LOGIN };
    NYA_SmtpMessage message = { .from = "a@b.test", .to = "c@d.test", .text = "body", .subject = "Hi" };

    nya_check(nya_smtp_send(arena, &config, &message).kind == NYA_ERROR_INVALID_ARGUMENT, "a login with no username or password is refused");
  }

  // TEST: the public send refuses an injected header before it opens a connection.
  {
    NYA_SmtpConfig config = {
      .host = "smtp.test", .port = 587, .security = NYA_SMTP_SECURITY_STARTTLS,
    };
    NYA_SmtpMessage message = {
      .from = "a@b.test", .to = "c@d.test", .text = "body", .subject = "Hi\r\nBcc: victim@evil.test",
    };

    NYA_Error refused = nya_smtp_send(arena, &config, &message);
    nya_check(!refused.ok && refused.kind == NYA_ERROR_INVALID_ARGUMENT, "a send with an injected header is refused before any socket is opened");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
