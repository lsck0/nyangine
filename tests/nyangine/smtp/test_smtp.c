/**
 * The SMTP client, without a network.
 *
 * Two things are proven here and both are deterministic under the sanitizers. The message builder is a
 * pure function, so its RFC 5322 output — the headers, the MIME framing, the base64 bodies — is asserted
 * directly, and a header value carrying a newline is shown to be refused rather than turned into a header
 * of its own. The conversation is run over a scripted in-memory channel that plays a fake server's canned
 * replies and records everything the client says, so the command sequence and the base64 of the AUTH
 * credentials are checked against exactly the bytes that would go on the wire, with no socket involved.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * A SCRIPTED FAKE SERVER OVER AN IN-MEMORY CHANNEL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
  NYA_SmtpChannel base;

  NYA_ConstCString* replies;
  u32               reply_count;
  u32               reply_index;

  NYA_String* sent;
  b8          upgraded;
} FakeServer;

static NYA_Error fake_write(NYA_SmtpChannel* self, const u8* data, u64 size) {
  FakeServer* server = (FakeServer*)self;
  for (u64 i = 0; i < size; i++) nya_string_push_back(server->sent, data[i]);
  return NYA_OK;
}

static NYA_Error fake_read(NYA_SmtpChannel* self, u8* out, u64 capacity, u64* out_read) {
  FakeServer* server = (FakeServer*)self;

  *out_read = 0;
  if (server->reply_index >= server->reply_count) return NYA_OK; // read as a close.

  NYA_ConstCString reply  = server->replies[server->reply_index++];
  u64              length = strlen(reply);
  nya_assert(length <= capacity, "a scripted reply is larger than the read buffer");

  nya_memcpy(out, reply, length);
  *out_read = length;

  return NYA_OK;
}

static NYA_Error fake_upgrade(NYA_SmtpChannel* self) {
  FakeServer* server = (FakeServer*)self;
  server->upgraded   = true;
  return NYA_OK;
}

/** Whether `needles` all appear in `haystack`, each after the previous one. */
static b8 in_order(NYA_ConstCString haystack, const NYA_ConstCString* needles, u32 count) {
  NYA_ConstCString cursor = haystack;

  for (u32 i = 0; i < count; i++) {
    NYA_ConstCString at = strstr(cursor, needles[i]);
    if (at == nullptr) return false;
    cursor = at + strlen(needles[i]);
  }

  return true;
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

  // TEST: the STARTTLS + AUTH LOGIN command sequence, over a scripted server.
  {
    NYA_ConstCString replies[] = {
      "220 smtp.test ESMTP\r\n",
      "250-smtp.test\r\n250 AUTH LOGIN PLAIN\r\n",
      "220 ready to start TLS\r\n",
      "250-smtp.test\r\n250 AUTH LOGIN PLAIN\r\n",
      "334 VXNlcm5hbWU6\r\n",
      "334 UGFzc3dvcmQ6\r\n",
      "235 2.7.0 authenticated\r\n",
      "250 2.1.0 sender ok\r\n",
      "250 2.1.5 recipient ok\r\n",
      "354 end data with <CRLF>.<CRLF>\r\n",
      "250 2.0.0 queued\r\n",
      "221 2.0.0 bye\r\n",
    };

    FakeServer server = {
      .base    = { .write = fake_write, .read = fake_read, .upgrade = fake_upgrade },
      .replies = replies,
      .reply_count = nya_carray_length(replies),
      .sent    = nya_string_create(arena),
    };

    NYA_SmtpConfig config = {
      .host = "smtp.test", .port = 587, .security = NYA_SMTP_SECURITY_STARTTLS,
      .auth = NYA_SMTP_AUTH_LOGIN, .username = "postmaster@example.com", .password = "hunter2",
    };
    NYA_SmtpMessage message = {
      .from = "noreply@example.com", .to = "person@elsewhere.test", .subject = "Hi", .text = "body",
    };

    NYA_String* blob = nullptr;
    nya_check(nya_smtp_message_build(arena, &message, &blob).ok, "the message builds");

    NYA_Error result = _nya_smtp_converse(arena, &config, &message, nya_string_to_cstring(arena, blob), &server.base);
    nya_check(result.ok, "the conversation completes: %s", (NYA_ConstCString)result.message);
    nya_check(server.upgraded, "STARTTLS upgraded the channel before authenticating");

    NYA_ConstCString sent = nya_string_to_cstring(arena, server.sent);

    NYA_ConstCString steps[] = {
      "EHLO example.com\r\n",
      "STARTTLS\r\n",
      "EHLO example.com\r\n",                                       // a second EHLO after the upgrade
      "AUTH LOGIN\r\n",
      nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s\r\n", base64_of(arena, "postmaster@example.com"))),
      nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s\r\n", base64_of(arena, "hunter2"))),
      "MAIL FROM:<noreply@example.com>\r\n",
      "RCPT TO:<person@elsewhere.test>\r\n",
      "DATA\r\n",
      "\r\n.\r\n",
      "QUIT\r\n",
    };
    nya_check(in_order(sent, steps, nya_carray_length(steps)), "the client speaks the SMTP steps in the right order with the credentials base64 encoded");

    // The password never appears in the clear, only as its base64 form.
    nya_check(strstr(sent, "hunter2") == nullptr, "the password is never sent in the clear");
  }

  // TEST: AUTH PLAIN sends one base64 token of NUL-separated credentials.
  {
    NYA_ConstCString replies[] = {
      "220 smtp.test ESMTP\r\n",
      "250 smtp.test\r\n",
      "235 2.7.0 authenticated\r\n",
      "250 sender ok\r\n",
      "250 recipient ok\r\n",
      "354 go\r\n",
      "250 queued\r\n",
      "221 bye\r\n",
    };

    FakeServer server = {
      .base    = { .write = fake_write, .read = fake_read, .upgrade = fake_upgrade },
      .replies = replies,
      .reply_count = nya_carray_length(replies),
      .sent    = nya_string_create(arena),
    };

    // Implicit TLS here: the channel is already encrypted, so there is no STARTTLS in the script.
    NYA_SmtpConfig config = {
      .host = "smtp.test", .port = 465, .security = NYA_SMTP_SECURITY_IMPLICIT,
      .auth = NYA_SMTP_AUTH_PLAIN, .username = "user", .password = "pass",
    };
    NYA_SmtpMessage message = {
      .from = "noreply@example.com", .to = "person@elsewhere.test", .subject = "Hi", .text = "body",
    };
    // A channel that reports itself as already-TLS: the conversation must not try to upgrade it.
    server.base.upgrade = nullptr;

    NYA_String* blob = nullptr;
    nya_check(nya_smtp_message_build(arena, &message, &blob).ok, "the message builds");

    NYA_Error result = _nya_smtp_converse(arena, &config, &message, nya_string_to_cstring(arena, blob), &server.base);
    nya_check(result.ok, "the AUTH PLAIN conversation completes: %s", (NYA_ConstCString)result.message);

    NYA_ConstCString sent = nya_string_to_cstring(arena, server.sent);

    // The token is authorize-id (empty), then the username and the password, NUL separated.
    u8 token[] = { 0, 'u', 's', 'e', 'r', 0, 'p', 'a', 's', 's' };
    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, token, sizeof(token));

    NYA_ConstCString expected = nya_string_to_cstring(arena, nya_string_sprintf(arena, "AUTH PLAIN %s\r\n", nya_string_to_cstring(arena, encoded)));
    nya_check(nya_string_contains(sent, expected), "AUTH PLAIN carries the base64 of the NUL-separated credentials");
    nya_check(strstr(sent, "STARTTLS") == nullptr, "implicit TLS does not send STARTTLS");
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
