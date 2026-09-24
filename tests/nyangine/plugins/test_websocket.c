/**
 * The websocket client: the framing on its own, the handshake against a server written here, and what
 * the client does when that server misbehaves.
 *
 * The server in this file is deliberately hand rolled and deliberately rude. It is the only way to
 * prove that a frame the client will never send is still handled when one arrives.
 **/

#include <time.h>

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** How long the two ends are pumped against each other before a step is called lost. */
#define PUMP_STEPS 4000

/** What the test server reads and writes at a time. */
#define SERVER_BUFFER_BYTES 8192

typedef struct {
  NYA_OsSocket      listener;
  NYA_OsSocket stream;

  u8  buffer[SERVER_BUFFER_BYTES];
  u64 size;

  b8 upgraded;

  /** Set once the server has been told to stop answering and start being rude. */
  b8 sent_close;
} Server;

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/* THE TEST SERVER */

/** Writes a server side frame: never masked, which is what RFC 6455 requires of a server. */
static void server_send(Server* server, u8 opcode, b8 fin, const u8* payload, u64 size) {
  nya_assert(server->stream.handle != 0);

  u8  header[10] = { 0 };
  u64 at         = 0;

  header[at++] = (u8)((fin ? 0x80U : 0U) | (opcode & 0x0FU));

  if (size < 126) {
    header[at++] = (u8)size;
  } else if (size <= 0xFFFFU) {
    header[at++] = 126;
    header[at++] = (u8)((size >> 8) & 0xFFU);
    header[at++] = (u8)(size & 0xFFU);
  } else {
    header[at++] = 127;
    for (s32 shift = 56; shift >= 0; shift -= 8) header[at++] = (u8)((size >> shift) & 0xFFU);
  }

  {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(server->stream, (const u8*)header, at, &wrote) == NYA_OS_SOCKET_OK);
  }
  if (size > 0) {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(server->stream, (const u8*)payload, size, &wrote) == NYA_OS_SOCKET_OK);
  }
}

/** Takes `count` bytes off the front of the server's buffer. */
static void server_consume(Server* server, u64 count) {
  nya_assert(count <= server->size);

  server->size -= count;
  if (server->size > 0) nya_memmove(server->buffer, server->buffer + count, server->size);
}

/** Answers the upgrade request, computing the accept with the same function the client checks it with. */
static void server_upgrade(Server* server, NYA_Arena* arena) {
  server->buffer[server->size] = '\0';

  NYA_ConstCString request = (NYA_ConstCString)server->buffer;

  NYA_ConstCString key_at = strstr(request, "Sec-WebSocket-Key: ");
  nya_assert(key_at != nullptr, "the client sent no key");

  key_at += strlen("Sec-WebSocket-Key: ");

  u64 key_length = 0;
  while (key_at[key_length] != '\r' && key_at[key_length] != '\n' && key_at[key_length] != '\0') key_length++;

  NYA_String* key = nya_string_create(arena);
  for (u64 i = 0; i < key_length; i++) nya_string_push_back(key, (u8)key_at[i]);

  char accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };
  NYA_EXPECT(nya_websocket_accept_from_key(nya_string_to_cstring(arena, key), accept));

  NYA_String* response = nya_string_sprintf(
      arena,
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n"
      "\r\n",
      accept
  );

  {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(server->stream, (const u8*)response->items, response->length, &wrote) == NYA_OS_SOCKET_OK);
  }

  server->upgraded = true;
}

/**
 * One step of the server: accept, read, and answer.
 *
 * Echoes text and binary back, answers a ping with a pong, and answers a close with a close. It
 * unmasks what the client sent, which is also what proves the client masked it.
 * */
static void server_pump(Server* server, NYA_Arena* arena) {
  if (server->stream.handle == 0) {
    {
      NYA_OsAddress      from   = { 0 };
      NYA_OsSocketStatus taken  = nya_os_socket_accept(server->listener, &server->stream, &from);

      nya_assert(taken == NYA_OS_SOCKET_OK || taken == NYA_OS_SOCKET_WOULD_BLOCK);
    }
    if (server->stream.handle == 0) return;
  }

  if (server->size < sizeof(server->buffer) - 1) {
    u64 got = 0;
    (void)nya_os_socket_receive(server->stream, server->buffer + server->size, sizeof(server->buffer) - 1 - server->size, &got);
    if (got > 0) server->size += (u64)got;
  }

  if (!server->upgraded) {
    if (server->size < 4) return;

    for (u64 i = 0; i + 3 < server->size; i++) {
      if (server->buffer[i] == '\r' && server->buffer[i + 1] == '\n' && server->buffer[i + 2] == '\r' && server->buffer[i + 3] == '\n') {
        server_upgrade(server, arena);
        server_consume(server, i + 4);
        break;
      }
    }

    return;
  }

  // Frames, decoded with the engine's own decoder: a server has to read exactly what a client writes.
  for (u32 step = 0; step < 16; step++) {
    NYA_WebSocketFrame frame = { 0 };

    if (nya_websocket_frame_decode(server->buffer, server->size, &frame) != NYA_WEBSOCKET_FRAME_OK) return;
    if (server->size < frame.header_size + frame.payload_size) return;

    nya_assert(frame.masked, "the client sent an unmasked frame, which RFC 6455 forbids");

    u8* payload = server->buffer + frame.header_size;
    for (u64 i = 0; i < frame.payload_size; i++) payload[i] ^= frame.mask[i & 3U];

    switch (frame.opcode) {
      case NYA_WEBSOCKET_OPCODE_TEXT:
      case NYA_WEBSOCKET_OPCODE_BINARY: server_send(server, (u8)frame.opcode, true, payload, frame.payload_size); break;
      case NYA_WEBSOCKET_OPCODE_PING:   server_send(server, NYA_WEBSOCKET_OPCODE_PONG, true, payload, frame.payload_size); break;

      case NYA_WEBSOCKET_OPCODE_CLOSE:  {
        if (!server->sent_close) {
          server->sent_close = true;
          server_send(server, NYA_WEBSOCKET_OPCODE_CLOSE, true, payload, frame.payload_size);
        }
      } break;

      default: break;
    }

    server_consume(server, frame.header_size + frame.payload_size);
  }
}

static void server_destroy(Server* server) {
  if (server->stream.handle != 0) nya_os_socket_close(server->stream);
  nya_os_socket_close(server->listener);

  *server = (Server){ 0 };
}

/* PUMPING BOTH ENDS */

typedef struct {
  u32 opens;
  u32 texts;
  u32 binaries;
  u32 pongs;
  u32 closes;

  NYA_WebSocketClose code;

  u8  last[1024];
  u64 last_size;
} Collected;

/** Steps both ends until `wanted` events of that kind have arrived, or the budget runs out. */
static void pump(NYA_WebSocket* socket, Server* server, NYA_Arena* arena, Collected* out, u32* counter, u32 wanted) {
  for (u32 step = 0; step < PUMP_STEPS && *counter < wanted; step++) {
    server_pump(server, arena);

    NYA_WebSocketEvent event = { 0 };

    while (nya_websocket_poll(socket, &event)) {
      switch (event.kind) {
        case NYA_WEBSOCKET_EVENT_OPEN:   out->opens++; break;

        case NYA_WEBSOCKET_EVENT_TEXT:
        case NYA_WEBSOCKET_EVENT_BINARY:
        case NYA_WEBSOCKET_EVENT_PONG:   {
          if (event.kind == NYA_WEBSOCKET_EVENT_TEXT) out->texts++;
          if (event.kind == NYA_WEBSOCKET_EVENT_BINARY) out->binaries++;
          if (event.kind == NYA_WEBSOCKET_EVENT_PONG) out->pongs++;

          out->last_size = nya_min(event.size, sizeof(out->last));
          if (out->last_size > 0) nya_memcpy(out->last, event.data, out->last_size);
        } break;

        case NYA_WEBSOCKET_EVENT_CLOSED: {
          out->closes++;
          out->code = event.code;
        } break;

        default: break;
      }
    }

    // A local connection needs no real waiting; this only keeps the loop from being a hot spin.
    sleep_ms(1);
  }
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  nya_assert(SDL_Init(0), "SDL_Init failed: %s", SDL_GetError());
  nya_assert(nya_os_socket_start() == NYA_OS_SOCKET_OK, "the host's socket library would not start");

  NYA_Arena* arena = nya_arena_create(.name = "test_websocket");
  defer      nya_arena_destroy(arena);

  printf("TEST: a frame header round trips, in all three length forms\n");
  {
    u8  mask[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    u64 sizes[] = { 0, 1, 125, 126, 127, 0xFFFF, 0x10000, 0x40000 };

    for (u32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
      u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
      u64 header_size                            = 0;

      NYA_EXPECT(nya_websocket_frame_encode(NYA_WEBSOCKET_OPCODE_BINARY, true, sizes[i], mask, header, &header_size));

      NYA_WebSocketFrame frame = { 0 };
      nya_assert(nya_websocket_frame_decode(header, header_size, &frame) == NYA_WEBSOCKET_FRAME_OK, "size " FMTu64 " did not decode", sizes[i]);

      nya_assert(frame.payload_size == sizes[i]);
      nya_assert(frame.header_size == header_size);
      nya_assert(frame.fin);
      nya_assert(frame.masked);
      nya_assert(frame.opcode == NYA_WEBSOCKET_OPCODE_BINARY);
      nya_assert(nya_memcmp(frame.mask, mask, 4) == 0);

      // Every prefix of a legal header is incomplete rather than wrong, which is what lets a reader ask again instead of dropping the connection on a short read.
      for (u64 shorter = 0; shorter < header_size; shorter++) {
        nya_assert(
            nya_websocket_frame_decode(header, shorter, &frame) == NYA_WEBSOCKET_FRAME_INCOMPLETE,
            "a %llu byte prefix was not incomplete",
            (unsigned long long)shorter
        );
      }
    }

    printf("  eight payload sizes round tripped, and every prefix of each read as incomplete\n");
  }

  printf("TEST: the decoder refuses what the protocol forbids\n");
  {
    NYA_WebSocketFrame frame = { 0 };

    // A reserved bit, with no extension negotiated to give it a meaning.
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0xC1, 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0xA1, 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x91, 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);

    // Opcodes nobody defined.
    for (u8 opcode = 0x3; opcode <= 0x7; opcode++) {
      nya_assert(nya_websocket_frame_decode((const u8[]){ (u8)(0x80U | opcode), 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    }
    for (u8 opcode = 0xB; opcode <= 0xF; opcode++) {
      nya_assert(nya_websocket_frame_decode((const u8[]){ (u8)(0x80U | opcode), 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    }

    // A control frame that is fragmented, and one that is too long to be a control frame.
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x09, 0x00 }, 2, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x89, 0x7E, 0x00, 0x7F }, 4, &frame) == NYA_WEBSOCKET_FRAME_INVALID);

    // Lengths not written in the shortest form the RFC demands.
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x82, 0x7E, 0x00, 0x7D }, 4, &frame) == NYA_WEBSOCKET_FRAME_INVALID);
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x82, 0x7F, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF }, 10, &frame) == NYA_WEBSOCKET_FRAME_INVALID);

    // A sixty four bit length with its top bit set, which the RFC reads as a negative number.
    nya_assert(nya_websocket_frame_decode((const u8[]){ 0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0 }, 10, &frame) == NYA_WEBSOCKET_FRAME_INVALID);

    // And the encoder refuses the same things from the other side.
    u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
    u64 header_size                            = 0;
    u8  mask[4]                                = { 0 };

    nya_assert(!nya_websocket_frame_encode((NYA_WebSocketOpcode)0x3, true, 0, mask, header, &header_size).ok);
    nya_assert(!nya_websocket_frame_encode(NYA_WEBSOCKET_OPCODE_PING, false, 0, mask, header, &header_size).ok);
    nya_assert(!nya_websocket_frame_encode(NYA_WEBSOCKET_OPCODE_PING, true, 126, mask, header, &header_size).ok);
    nya_assert(!nya_websocket_frame_encode(NYA_WEBSOCKET_OPCODE_BINARY, true, U64_MAX, mask, header, &header_size).ok);

    printf("  eighteen illegal frames refused by the decoder, four by the encoder\n");
  }

  printf("TEST: the accept key is the one RFC 6455 writes down\n");
  {
    char accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };

    // Section 1.3's worked example, which is the only known answer either end can be checked against.
    NYA_EXPECT(nya_websocket_accept_from_key("dGhlIHNhbXBsZSBub25jZQ==", accept));
    nya_assert(nya_string_equals((NYA_ConstCString)accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "got '%s'", accept);

    nya_assert(!nya_websocket_accept_from_key(nullptr, accept).ok);
    nya_assert(!nya_websocket_accept_from_key("", accept).ok);
    nya_assert(!nya_websocket_accept_from_key("too short", accept).ok);

    printf("  the RFC's example matched, and three bad keys were refused\n");
  }

  printf("TEST: a url is parsed or the socket is never created\n");
  {
    NYA_ConstCString refused[] = {
      nullptr,
      "",
      "http://example.com",
      "example.com",
      "ws://",
      "ws://user:password@example.com",
      "ws://example.com:0",
      "ws://example.com:99999",
      "ws://example.com:abc",
      "ws://example.com nope",
      "ws://example.com/path with a space",
      "ws://example.com/#top",
      "ws://0x7f.1/",
    };

    for (u32 i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
      NYA_WebSocket* socket = nullptr;
      NYA_Error      result = nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = refused[i] }, &socket);

      nya_assert(!result.ok, "'%s' was accepted as a url", refused[i] == nullptr ? "(null)" : refused[i]);
      nya_assert(socket == nullptr);
    }

    // A header the handshake owns cannot be set, because setting it would break the handshake.
    NYA_WebSocket* socket = nullptr;
    nya_assert(!nya_websocket_create(
                    arena,
                    (NYA_WebSocketOptions){ .url = "ws://127.0.0.1:1", .headers = { { .name = "Sec-WebSocket-Key", .value = "x" } } },
                    &socket
    )
                    .ok);

    // And neither can a header that carries a newline, which would append headers of its own.
    nya_assert(!nya_websocket_create(
                    arena,
                    (NYA_WebSocketOptions){ .url = "ws://127.0.0.1:1", .headers = { { .name = "X-Thing", .value = "a\r\nX-Other: b" } } },
                    &socket
    )
                    .ok);

    // Destroying nothing is a no-op, so this pairs with a failed create.
    nya_websocket_destroy(nullptr);

    printf("  thirteen bad urls and two bad headers refused\n");
  }

  printf("TEST: the whole exchange against a server\n");
  {
    Server server = { 0 };
    u16    port   = 0;

    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    nya_assert(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, port, 0, &server.listener) == NYA_OS_SOCKET_OK, "the listener would not open");
    nya_assert(server.listener.handle != 0, "could not listen on port %u: %s", (u32)port, SDL_GetError());
    defer server_destroy(&server);

    NYA_String* url = nya_string_sprintf(arena, "ws://127.0.0.1:%u/socket", (unsigned)port);

    NYA_WebSocket* socket = nullptr;
    NYA_EXPECT(nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = nya_string_to_cstring(arena, url) }, &socket));
    defer nya_websocket_destroy(socket);

    nya_assert(nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_CONNECTING);

    Collected collected = { 0 };
    pump(socket, &server, arena, &collected, &collected.opens, 1);

    nya_assert(collected.opens == 1, "the upgrade never completed");
    nya_assert(nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_OPEN);

    NYA_EXPECT(nya_websocket_send_text(socket, "hello obs"));
    pump(socket, &server, arena, &collected, &collected.texts, 1);

    nya_assert(collected.texts == 1, "the text message never came back");
    nya_assert(collected.last_size == 9);
    nya_assert(nya_memcmp(collected.last, "hello obs", 9) == 0);

    // An object, which is what a control protocol actually sends.
    NYA_Object* body = nya_object_create(arena);
    nya_object_add(body, "op", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 6 });

    NYA_EXPECT(nya_websocket_send_object(socket, arena, body));
    pump(socket, &server, arena, &collected, &collected.texts, 2);

    nya_assert(collected.texts == 2, "the object never came back");

    NYA_Object* echoed = nullptr;
    NYA_EXPECT(nya_deserialize(arena, collected.last, collected.last_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &echoed));
    nya_assert(nya_object_get(echoed, "op")->as_s64 == 6);

    u8 bytes[] = { 0x00, 0xFF, 0x7F, 0x80, 0x01 };
    NYA_EXPECT(nya_websocket_send_binary(socket, bytes, sizeof(bytes)));
    pump(socket, &server, arena, &collected, &collected.binaries, 1);

    nya_assert(collected.binaries == 1, "the binary message never came back");
    nya_assert(collected.last_size == sizeof(bytes));
    nya_assert(nya_memcmp(collected.last, bytes, sizeof(bytes)) == 0);

    NYA_EXPECT(nya_websocket_ping(socket, (const u8*)"beat", 4));
    pump(socket, &server, arena, &collected, &collected.pongs, 1);

    nya_assert(collected.pongs == 1, "the ping was never answered");
    nya_assert(collected.last_size == 4 && nya_memcmp(collected.last, "beat", 4) == 0);

    // A message the server sends in pieces arrives as one.
    server_send(&server, NYA_WEBSOCKET_OPCODE_TEXT, false, (const u8*)"frag", 4);
    server_send(&server, NYA_WEBSOCKET_OPCODE_CONTINUATION, false, (const u8*)"men", 3);
    server_send(&server, NYA_WEBSOCKET_OPCODE_CONTINUATION, true, (const u8*)"ted", 3);

    pump(socket, &server, arena, &collected, &collected.texts, 3);

    nya_assert(collected.texts == 3, "the fragmented message never arrived");
    nya_assert(collected.last_size == 10);
    nya_assert(nya_memcmp(collected.last, "fragmented", 10) == 0);

    // The server answers a ping with a pong even when the client never asked for one, which the client has to accept without reporting anything.
    server_send(&server, NYA_WEBSOCKET_OPCODE_PING, true, (const u8*)"srv", 3);
    pump(socket, &server, arena, &collected, &collected.pongs, 2);

    NYA_EXPECT(nya_websocket_close(socket, NYA_WEBSOCKET_CLOSE_NORMAL, "done"));
    pump(socket, &server, arena, &collected, &collected.closes, 1);

    nya_assert(collected.closes == 1, "the close was never answered");
    nya_assert(collected.code == NYA_WEBSOCKET_CLOSE_NORMAL, "closed with %s", nya_websocket_close_name(collected.code));
    nya_assert(nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_CLOSED);

    // Everything is refused once it is closed, and closing again is a no-op.
    nya_assert(!nya_websocket_send_text(socket, "too late").ok);
    NYA_EXPECT(nya_websocket_close(socket, NYA_WEBSOCKET_CLOSE_NORMAL, nullptr));

    printf("  upgraded, echoed text, an object and binary, answered a ping, joined three fragments, and closed\n");
  }

  printf("TEST: a rude server is closed rather than believed\n");
  {
    // Three servers, each breaking one rule, because a closed socket never re-opens.
    struct {
      NYA_ConstCString   what;
      NYA_WebSocketClose expected;
    } cases[] = {
      { "a masked frame from a server", NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR  },
      { "a message past the ceiling",   NYA_WEBSOCKET_CLOSE_TOO_LARGE       },
      { "text that is not utf-8",       NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD },
    };

    for (u32 which = 0; which < sizeof(cases) / sizeof(cases[0]); which++) {
      Server server = { 0 };
      u16    port   = 0;

      NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

      nya_assert(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, port, 0, &server.listener) == NYA_OS_SOCKET_OK, "the listener would not open");
      nya_assert(server.listener.handle != 0, "could not listen on port %u: %s", (u32)port, SDL_GetError());

      NYA_String* url = nya_string_sprintf(arena, "ws://127.0.0.1:%u/", (unsigned)port);

      NYA_WebSocket* socket = nullptr;
      NYA_EXPECT(nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = nya_string_to_cstring(arena, url), .max_message_bytes = 4096 }, &socket));

      Collected collected = { 0 };
      pump(socket, &server, arena, &collected, &collected.opens, 1);
      nya_assert(collected.opens == 1, "the upgrade never completed for '%s'", cases[which].what);

      switch (which) {
        case 0: {
          // A server frame with the mask bit set, which RFC 6455 section 5.1 forbids outright.
          u8 frame[] = { 0x81, 0x84, 0x01, 0x02, 0x03, 0x04, 'a' ^ 1, 'b' ^ 2, 'c' ^ 3, 'd' ^ 4 };
          {
          u64 wrote = 0;
          nya_assert(nya_os_socket_send(server.stream, (const u8*)frame, sizeof(frame), &wrote) == NYA_OS_SOCKET_OK);
        }
        } break;

        case 1: {
          // A length past this socket's ceiling, announced but never sent: the close happens on the header alone, so not one byte of the body is ever kept.
          u8 frame[] = { 0x82, 0x7F, 0, 0, 0, 0, 0, 0x10, 0, 0 };
          {
          u64 wrote = 0;
          nya_assert(nya_os_socket_send(server.stream, (const u8*)frame, sizeof(frame), &wrote) == NYA_OS_SOCKET_OK);
        }
        } break;

        default: {
          u8 invalid[] = { 0xC3, 0x28 };
          server_send(&server, NYA_WEBSOCKET_OPCODE_TEXT, true, invalid, sizeof(invalid));
        } break;
      }

      pump(socket, &server, arena, &collected, &collected.closes, 1);

      nya_assert(collected.closes == 1, "'%s' did not close the socket", cases[which].what);
      nya_assert(collected.code == cases[which].expected, "'%s' closed with %s", cases[which].what, nya_websocket_close_name(collected.code));

      nya_websocket_destroy(socket);
      server_destroy(&server);
    }

    printf("  three rule breaking servers each closed the socket with the right code\n");
  }

  printf("TEST: a server that is not there is an event, not a failure\n");
  {
    NYA_WebSocket* socket = nullptr;

    // Nothing listens on this port, and the create still succeeds because the connect has not happened yet. The refusal arrives as a CLOSED event, which is the whole point of the state machine.
    NYA_EXPECT(nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = "ws://127.0.0.1:1", .handshake_timeout_ms = 2000 }, &socket));
    defer nya_websocket_destroy(socket);

    u32 closes = 0;

    for (u32 step = 0; step < PUMP_STEPS && closes == 0; step++) {
      NYA_WebSocketEvent event = { 0 };

      while (nya_websocket_poll(socket, &event)) {
        if (event.kind == NYA_WEBSOCKET_EVENT_CLOSED) closes++;
      }

      sleep_ms(1);
    }

    nya_assert(closes == 1, "an unreachable server never reported a close");
    nya_assert(nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_CLOSED);

    // And the close is reported exactly once.
    NYA_WebSocketEvent event = { 0 };
    nya_assert(!nya_websocket_poll(socket, &event), "the close was reported twice");

    printf("  a refused connection arrived as one CLOSED event\n");
  }

  nya_os_socket_stop();

  printf("PASSED: test_websocket (0 failures)\n");

  return EXIT_SUCCESS;
}
