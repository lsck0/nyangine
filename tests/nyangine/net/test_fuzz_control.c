/**
 * The two decoders that face a process other than this one: the control socket's framing and message
 * layer, and the websocket client's framing. Both are fed mutations of valid input and plain noise
 * from a fixed seed.
 *
 * ASan and UBSan are the assertions here, as in test_fuzz.c: nothing may read out of bounds, overflow,
 * or size an allocation from a number a stranger chose. The explicit assertions are the invariants
 * each decoder promises, checked on everything it accepts.
 **/

#include <time.h>
#include <unistd.h>

#include "SDL3/SDL_init.h"

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define FUZZ_INPUT_MAX 2048

/*
 * A reflection written out by hand, for the same reason test_control.c does it: the generated tables
 * are compiled with the game rather than with the engine, so a test cannot name nya_reflect_of.
 */

typedef struct {
  u32 index;
  f32 amount;
} Knobs;

static const NYA_TypeReflection R_U32 = { .name      = "u32",
                                          .kind      = NYA_REFLECT_PRIMITIVE,
                                          .size      = sizeof(u32),
                                          .alignment = alignof(u32),
                                          .primitive = NYA_TYPE_U32 };
static const NYA_TypeReflection R_F32 = { .name      = "f32",
                                          .kind      = NYA_REFLECT_PRIMITIVE,
                                          .size      = sizeof(f32),
                                          .alignment = alignof(f32),
                                          .primitive = NYA_TYPE_F32 };

static const NYA_ReflectField R_KNOBS_FIELDS[] = {
  { .name = "index",  .type = &R_U32, .offset = offsetof(Knobs, index)  },
  { .name = "amount", .type = &R_F32, .offset = offsetof(Knobs, amount) },
};

static const NYA_TypeReflection R_KNOBS = {
  .name        = "Knobs",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(Knobs),
  .alignment   = alignof(Knobs),
  .fields      = R_KNOBS_FIELDS,
  .field_count = 2,
};

/** xorshift, so a failure replays exactly from the seed. */
static u64 SEED = 0x6E7961636F6E7472ULL;

static u64 roll(void) {
  SEED ^= SEED << 13;
  SEED ^= SEED >> 7;
  SEED ^= SEED << 17;
  return SEED;
}

static u32 below(u32 limit) {
  nya_assert(limit > 0);
  return (u32)(roll() % limit);
}

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/**
 * Mutates `data` in place, a few edits at a time: flipped bits, bytes set to edge values, runs erased
 * or duplicated, and the end moved. Returns the new size, never past `capacity`.
 *
 * The same shape as the mutator in test_fuzz.c, because a second one that drifts would mean two
 * different ideas of what a mutation is.
 * */
static u64 mutate(u8* data, u64 size, u64 capacity) {
  static const u8 edges[] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };

  u32 edits = 1 + below(4);

  for (u32 edit = 0; edit < edits; edit++) {
    switch (below(6)) {
      case 0:
        if (size > 0) data[below((u32)size)] ^= (u8)(1U << below(8));
        break;
      case 1:
        if (size > 0) data[below((u32)size)] = edges[below(sizeof(edges))];
        break;
      case 2:
        if (size > 0) data[below((u32)size)] = (u8)roll();
        break;

      case 3: {
        if (size < 2) break;
        u32 at  = below((u32)size);
        u32 run = 1 + below((u32)(size - at));
        nya_memmove(data + at, data + at + run, size - at - run);
        size -= run;
      } break;

      case 4: {
        if (size == 0 || size >= capacity) break;
        u32 at  = below((u32)size);
        u32 run = 1 + below((u32)nya_min(size - at, capacity - size));
        nya_memmove(data + at + run, data + at, size - at);
        size += run;
      } break;

      default: size = below((u32)size + 1); break;
    }
  }

  return size;
}

/* A SERVER THAT ONLY EVER UPGRADES */

/** Answers one upgrade request and then writes whatever it is told to. */
typedef struct {
  NYA_OsSocket      listener;
  NYA_OsSocket stream;

  u8  buffer[4096];
  u64 size;

  b8 upgraded;
} Upgrader;

static void upgrader_pump(Upgrader* server, NYA_Arena* arena) {
  if (server->stream.handle == 0) {
    {
      NYA_OsAddress from = { 0 };
      (void)nya_os_socket_accept(server->listener, &server->stream, &from);
    }
    if (server->stream.handle == 0) return;
  }

  if (server->upgraded) {
    // Everything after the upgrade is read and thrown away: this server never answers a frame, it only sends the ones the fuzzer hands it.
    u8 ignored[1024];
    {
      u64 ignored_size = 0;
      (void)nya_os_socket_receive(server->stream, ignored, sizeof(ignored), &ignored_size);
    }
    return;
  }

  if (server->size < sizeof(server->buffer) - 1) {
    u64 got = 0;
    (void)nya_os_socket_receive(server->stream, server->buffer + server->size, sizeof(server->buffer) - 1 - server->size, &got);
    if (got > 0) server->size += (u64)got;
  }

  server->buffer[server->size] = '\0';

  NYA_ConstCString end = strstr((NYA_ConstCString)server->buffer, "\r\n\r\n");
  if (end == nullptr) return;

  NYA_ConstCString key_at = strstr((NYA_ConstCString)server->buffer, "Sec-WebSocket-Key: ");
  nya_assert(key_at != nullptr);

  key_at += strlen("Sec-WebSocket-Key: ");

  NYA_String* key = nya_string_create(arena);
  for (u64 i = 0; key_at[i] != '\r' && key_at[i] != '\0'; i++) nya_string_push_back(key, (u8)key_at[i]);

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
  server->size     = 0;
}

static void upgrader_destroy(Upgrader* server) {
  if (server->stream.handle != 0) nya_os_socket_close(server->stream);
  nya_os_socket_close(server->listener);

  *server = (Upgrader){ 0 };
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  nya_assert(SDL_Init(0), "SDL_Init failed: %s", SDL_GetError());
  nya_assert(nya_os_socket_start() == NYA_OS_SOCKET_OK, "the host's socket library would not start");

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  NYA_EXPECT(nya_system_events_init());
  defer nya_system_events_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_fuzz_control");
  defer      nya_arena_destroy(arena);

  // Dropping a misbehaving peer is a warning, and this file makes tens of thousands of them do it on purpose. The warnings are the normal outcome here, so they are silenced rather than scrolled past.
  nya_log_level_set(NYA_LOG_LEVEL_ERROR);

  u8 input[FUZZ_INPUT_MAX];

  printf("TEST: the websocket frame decoder, on noise\n");
  {
    u32 accepted = 0;

    for (u32 iteration = 0; iteration < 400000; iteration++) {
      u64 size = below(NYA_WEBSOCKET_MAX_HEADER_BYTES + 4);
      for (u64 i = 0; i < size; i++) input[i] = (u8)roll();

      NYA_WebSocketFrame       frame  = { 0 };
      NYA_WebSocketFrameResult result = nya_websocket_frame_decode(input, size, &frame);

      nya_assert(result < NYA_WEBSOCKET_FRAME_RESULT_COUNT);
      if (result != NYA_WEBSOCKET_FRAME_OK) continue;

      accepted++;

      // Everything the header promises about a frame it accepted.
      nya_assert(frame.header_size >= 2 && frame.header_size <= NYA_WEBSOCKET_MAX_HEADER_BYTES);
      nya_assert(frame.header_size <= size);
      nya_assert((frame.payload_size >> 63) == 0, "a payload length with the top bit set was accepted");

      b8 control = ((u32)frame.opcode & 0x8U) != 0;

      nya_assert(!control || frame.fin);
      nya_assert(!control || frame.payload_size <= NYA_WEBSOCKET_MAX_CONTROL_BYTES);

      // The shortest encoding, which is the rule a lazy decoder drops first.
      if (frame.header_size >= 4 && (input[1] & 0x7FU) == 126) nya_assert(frame.payload_size >= 126);
      if ((input[1] & 0x7FU) == 127) nya_assert(frame.payload_size > 0xFFFFU);

      nya_assert(frame.masked == ((input[1] & 0x80U) != 0));
    }

    printf("  400000 noise headers, %u of them legal, every one of those well formed\n", accepted);
  }

  printf("TEST: the control framing, on noise\n");
  {
    for (u32 iteration = 0; iteration < 200000; iteration++) {
      u64 size = below(NYA_CONTROL_HEADER_BYTES + 4);
      for (u64 i = 0; i < size; i++) input[i] = (u8)roll();

      u64 length = 0;

      if (!nya_control_frame_decode(input, size, &length)) {
        nya_assert(length == 0, "a refused header still wrote a length");
        nya_assert(size < NYA_CONTROL_HEADER_BYTES, "a whole header was refused");
        continue;
      }

      // The header is four bytes, so the length it can name is bounded by construction. That is the property the control layer leans on when it decides whether to keep reading.
      nya_assert(length <= 0xFFFFFFFFULL);

      u8  written[NYA_CONTROL_HEADER_BYTES] = { 0 };
      u64 again                             = 0;

      // Anything inside the limit round trips; anything past it is refused by the encoder, which is where that limit is enforced for outgoing messages.
      if (length >= 1 && length <= NYA_CONTROL_MAX_MESSAGE_BYTES) {
        NYA_EXPECT(nya_control_frame_encode(length, written));
        nya_assert(nya_control_frame_decode(written, sizeof(written), &again) && again == length);
      } else {
        nya_assert(!nya_control_frame_encode(length, written).ok);
      }
    }

    printf("  200000 noise headers, every legal length round tripped and every illegal one refused\n");
  }

  printf("TEST: what the control surface makes of a hostile peer\n");
  {
    NYA_String* pid  = nya_string_sprintf(arena, "nya-fuzz-control-%d", (int)getpid());
    NYA_IpcName name = { 0 };
    NYA_EXPECT(nya_ipc_name_parse(nya_string_to_cstring(arena, pid), &name));

    NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){ .name = name, .permissions = NYA_CONTROL_PERMISSION_ALL }));
    defer nya_system_control_deinit();

    Knobs exposed = { 0 };
    NYA_EXPECT(nya_control_expose("peer", &R_KNOBS, &exposed));

    // Valid requests, one per verb, which the mutator then breaks in every way it can.
    NYA_ConstCString seeds[] = {
      "{\"op\":\"hello\",\"id\":1}",
      "{\"op\":\"object.list\"}",
      "{\"op\":\"object.get\",\"name\":\"peer\"}",
      "{\"op\":\"object.set\",\"name\":\"peer\",\"value\":{\"index\":3,\"amount\":0.5}}",
      "{\"op\":\"event.dispatch\",\"type\":\"CONTROL_MESSAGE\",\"name\":\"poke\",\"body\":{\"amount\":1}}",
      "{\"op\":\"event.subscribe\",\"types\":[\"QUIT\",\"KEY_DOWN\"]}",
      "{\"op\":\"event.unsubscribe\",\"types\":[\"QUIT\"]}",
      "nya 2 0\n{\n    op: string \"hello\";\n}\n",
    };

    NYA_IpcClient* client   = nullptr;
    u32            reopened = 0;
    u32            sent     = 0;

    for (u32 iteration = 0; iteration < 20000; iteration++) {
      if (client == nullptr || !nya_ipc_client_is_connected(client)) {
        if (client != nullptr) {
          nya_ipc_client_destroy(client);
          reopened++;
        }

        // The listener has to notice the old peer is gone before the new one fits.
        for (u32 step = 0; step < 8; step++) nya_system_control_tick();

        NYA_EXPECT(nya_ipc_client_create(arena, name, &client));
        for (u32 step = 0; step < 4; step++) nya_system_control_tick();
      }

      NYA_ConstCString seed = seeds[below(sizeof(seeds) / sizeof(seeds[0]))];
      u64              size = strlen(seed);

      nya_assert(size < FUZZ_INPUT_MAX);
      nya_memcpy(input, seed, size);

      // One in sixteen is pure noise rather than a mutation, so the parser sees bytes no document ever had as well as documents that are nearly right.
      if (iteration % 16 == 0) {
        size = below(256);
        for (u64 i = 0; i < size; i++) input[i] = (u8)roll();
      } else {
        size = mutate(input, size, FUZZ_INPUT_MAX);
      }

      if (size == 0) continue;

      u8 header[NYA_CONTROL_HEADER_BYTES] = { 0 };

      // Half the time the length is honest and half the time it is whatever the mutator produced, so the assembly path sees both a truncated message and one that claims more than it sends.
      u64 announced = (iteration & 1) ? size : (u64)below(0xFFFFU) + 1;

      if (!nya_control_frame_encode(announced, header).ok) continue;

      if (!nya_ipc_client_send(client, header, sizeof(header)).ok) continue;
      if (!nya_ipc_client_send(client, input, size).ok) continue;

      sent++;

      // Drained, and whatever comes back is read and thrown away: what matters is that the process is still here and the surface is still listening.
      for (u32 step = 0; step < 4; step++) nya_system_control_tick();

      u8  ignored[2048] = { 0 };
      u64 got           = 0;
      (void)nya_ipc_client_receive(client, ignored, sizeof(ignored), &got);

      nya_assert(nya_control_is_running(), "the control surface stopped listening");
      nya_assert(nya_control_connection_count() <= NYA_IPC_MAX_CONNECTIONS);
    }

    if (client != nullptr) nya_ipc_client_destroy(client);

    // The dispatches the fuzzer got through left events behind.
    NYA_Event drained = { 0 };
    while (nya_system_event_poll(&drained)) {}

    printf("  %u mutated requests survived; the peer was dropped and reconnected %u times\n", sent, reopened);
  }

  printf("TEST: what the websocket client makes of a hostile server\n");
  {
    Upgrader server = { 0 };
    u16      port   = 0;

    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    nya_assert(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, port, 0, &server.listener) == NYA_OS_SOCKET_OK, "the listener would not open");
    nya_assert(server.listener.handle != 0, "could not listen on port %u: %s", (u32)port, SDL_GetError());
    defer upgrader_destroy(&server);

    NYA_String* url = nya_string_sprintf(arena, "ws://127.0.0.1:%u/", (unsigned)port);

    /* A run is one socket fed a stream of frames the mutator produced, until the client closes it. A closed socket never reopens, so a new one is created for the next run; the count is what makes this a fuzz rather than one unlucky frame. */
    u32 runs     = 0;
    u32 frames   = 0;
    u32 messages = 0;

    while (runs < 400) {
      runs++;

      NYA_WebSocket* socket = nullptr;
      NYA_EXPECT(nya_websocket_create(
          arena,
          (NYA_WebSocketOptions){ .url = nya_string_to_cstring(arena, url), .max_message_bytes = 8192, .handshake_timeout_ms = 4000 },
          &socket
      ));

      b8 open = false;

      for (u32 step = 0; step < 2000 && !open; step++) {
        upgrader_pump(&server, arena);

        NYA_WebSocketEvent event = { 0 };

        while (nya_websocket_poll(socket, &event)) {
          if (event.kind == NYA_WEBSOCKET_EVENT_OPEN) open = true;
          if (event.kind == NYA_WEBSOCKET_EVENT_CLOSED) break;
        }

        if (nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_CLOSED) break;

        sleep_ms(1);
      }

      // A run whose upgrade did not complete is not a useful run; the socket is dropped and the server is rebuilt so the next one starts clean.
      if (!open) {
        nya_websocket_destroy(socket);
        upgrader_destroy(&server);

        nya_assert(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, port, 0, &server.listener) == NYA_OS_SOCKET_OK, "the listener would not open");
        nya_assert(server.listener.handle != 0);
        continue;
      }

      for (u32 burst = 0; burst < 24 && nya_websocket_state(socket) != NYA_WEBSOCKET_STATE_CLOSED; burst++) {
        /* A frame built the way a server would build one, then broken. The header is written by hand rather than by nya_websocket_frame_encode, because the encoder refuses exactly the frames this test wants on the wire. */
        u64 size      = 0;
        u8  opcodes[] = { 0x0, 0x1, 0x2, 0x8, 0x9, 0xA, 0x3, 0xB };
        u8  opcode    = opcodes[below(sizeof(opcodes))];
        u64 payload   = below(200);

        input[size++] = (u8)((below(2) == 0 ? 0x80U : 0U) | (u32)opcode | (below(8) == 0 ? (u32)(1U << (5 + below(3))) : 0U));

        if (payload < 126) {
          input[size++] = (u8)((below(8) == 0 ? 0x80U : 0U) | (u32)payload);
        } else {
          input[size++] = (u8)((below(8) == 0 ? 0x80U : 0U) | 126U);
          input[size++] = (u8)((payload >> 8) & 0xFFU);
          input[size++] = (u8)(payload & 0xFFU);
        }

        for (u64 i = 0; i < payload && size < FUZZ_INPUT_MAX; i++) input[size++] = (u8)roll();

        size = (burst % 4 == 0) ? mutate(input, size, FUZZ_INPUT_MAX) : size;
        if (size == 0) continue;

        {
        u64 wrote = 0;
        if (nya_os_socket_send(server.stream, input, size, &wrote) != NYA_OS_SOCKET_OK) break;
      }

        frames++;

        for (u32 step = 0; step < 32; step++) {
          upgrader_pump(&server, arena);

          NYA_WebSocketEvent event = { 0 };

          while (nya_websocket_poll(socket, &event)) {
            if (event.kind == NYA_WEBSOCKET_EVENT_TEXT || event.kind == NYA_WEBSOCKET_EVENT_BINARY) {
              messages++;

              // Whatever came out is inside the ceiling this socket was given, and a text message is terminated so it can be used as a C string.
              nya_assert(event.size <= 8192, "a message past the ceiling was handed out");
              if (event.kind == NYA_WEBSOCKET_EVENT_TEXT) nya_assert(event.data[event.size] == '\0');
            }

            if (event.kind == NYA_WEBSOCKET_EVENT_CLOSED) { nya_assert(event.reason != nullptr, "a close with no reason"); }
          }

          if (nya_websocket_state(socket) == NYA_WEBSOCKET_STATE_CLOSED) break;
        }
      }

      nya_websocket_destroy(socket);

      // The server end goes with it: this one only ever upgrades once.
      upgrader_destroy(&server);

      nya_assert(nya_os_socket_open(NYA_OS_SOCKET_LISTENER, port, 0, &server.listener) == NYA_OS_SOCKET_OK, "the listener would not open");
      nya_assert(server.listener.handle != 0);
    }

    printf("  %u sockets fed %u broken frames; %u messages came out and every one was well formed\n", runs, frames, messages);
  }

  nya_os_socket_stop();

  printf("PASSED: test_fuzz_control (0 failures)\n");

  return EXIT_SUCCESS;
}
