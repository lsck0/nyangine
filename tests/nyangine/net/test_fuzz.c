/**
 * Every wire decoder, fed mutations of valid input and plain noise from a fixed seed. ASan and UBSan are the
 * assertions: nothing may read out of bounds, overflow, or allocate from a size it was handed.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3_net/SDL_net.h"

#define FLAG_REPLICATED (1ULL << 2)

#define FUZZ_INPUT_MAX 1400

/** xorshift, so a failure replays exactly from the seed. */
static u64 SEED = 0x6E79616E67696E65ULL;

static u64 roll(void) {
  SEED ^= SEED << 13;
  SEED ^= SEED >> 7;
  SEED ^= SEED << 17;
  return SEED;
}

static u32 below(u32 limit) {
  return (u32)(roll() % limit);
}

/**
 * Mutates `data` in place, a few edits at a time: flipped bits, bytes set to edge values, runs erased or duplicated,
 * and the end moved. Returns the new size, never past `capacity`.
 * */
static u64 mutate(u8* data, u64 size, u64 capacity) {
  static const u8 edges[] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };

  u32 edits = 1 + below(4);

  for (u32 edit = 0; edit < edits; edit++) {
    switch (below(6)) {
      case 0: if (size > 0) data[below((u32)size)] ^= (u8)(1U << below(8)); break;
      case 1: if (size > 0) data[below((u32)size)] = edges[below(sizeof(edges))]; break;
      case 2: if (size > 0) data[below((u32)size)] = (u8)roll(); break;

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

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(peer, name);
  return nya_entity_spawn(.flags = FLAG_REPLICATED);
}

static void apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  entity->position.x += command->analog * delta_time_s;
}

static void sample_command(OUT NYA_NetCommand* command) {
  command->actions = 1;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());
  nya_assert(NET_Init(), "NET_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);
  defer nya_world_destroy(world);

  NYA_Arena* arena = nya_arena_create(.name = "test_fuzz");
  defer      nya_arena_destroy(arena);

  u8 input[FUZZ_INPUT_MAX];

  printf("TEST: snapshot decoding, with and without a baseline\n");
  {
    NYA_NetEntityState states[12] = { 0 };
    for (u32 i = 0; i < 12; i++) {
      states[i] = (NYA_NetEntityState){
        .handle   = { .index = 3 + (i * 5), .generation = 1 + i },
        .type     = i,
        .flags    = 1ULL << i,
        .position = { (f32)i * 13.5F, -(f32)i, 1000.0F },
        .velocity = { 3.0F, 0.0F, (f32)i },
        .scale    = { 1.0F, 2.0F, 1.0F },
        .rotation = nya_quaternion_from_euler(0.1F * (f32)i, 0.2F, 0.3F),
      };
    }

    NYA_NetSnapshot baseline_sent = { .tick = 40, .entities = states, .entity_count = 12 };

    NYA_String* full = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &baseline_sent, nullptr, full));

    NYA_NetSnapshot baseline = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, full->items, full->length, nullptr, &baseline));

    NYA_NetEntityState moved[12];
    nya_memcpy(moved, states, sizeof(moved));
    for (u32 i = 0; i < 12; i += 3) moved[i].position.x += 7.0F;
    moved[5].handle.generation++;

    NYA_NetSnapshot current = { .tick = 44, .command_tick = 900, .entities = moved + 1, .entity_count = 11 };

    NYA_String* delta = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &current, &baseline, delta));

    u32 accepted = 0;

    for (u32 iteration = 0; iteration < 40000; iteration++) {
      const NYA_String* seed = (iteration & 1) ? delta : full;
      u64               size = seed->length;

      nya_memcpy(input, seed->items, size);
      size = iteration % 16 == 0 ? below(FUZZ_INPUT_MAX) : mutate(input, size, FUZZ_INPUT_MAX);
      if (iteration % 16 == 0) for (u64 i = 0; i < size; i++) input[i] = (u8)roll();

      NYA_Arena* scratch = nya_arena_create(.name = "fuzz_snapshot");

      u64 tick = 0, baseline_tick = 0;
      (void)nya_net_snapshot_peek(input, size, &tick, &baseline_tick);

      NYA_NetSnapshot decoded = { 0 };
      if (nya_net_snapshot_decode(scratch, input, size, (iteration & 2) ? &baseline : nullptr, &decoded).ok) {
        accepted++;

        nya_assert(decoded.entity_count <= NYA_NET_MAX_REPLICATED);
        for (u32 i = 1; i < decoded.entity_count; i++) nya_assert(decoded.entities[i - 1].handle.index < decoded.entities[i].handle.index, "a decoded snapshot out of order");
        for (u32 i = 0; i < decoded.entity_count; i++) nya_assert(decoded.entities[i].handle.index < NYA_ENTITY_MAX && decoded.entities[i].handle.generation != 0);
      }

      nya_arena_destroy(scratch);
    }

    printf("  40000 mutated snapshots, %u decoded, every one well formed\n", accepted);
  }

  printf("TEST: command runs and documents\n");
  {
    NYA_NetCommand run[NYA_NET_COMMAND_REDUNDANCY] = {
      { .tick = 70, .actions = 3, .aim = { 1.0F, 2.0F }, .analog = 0.5F },
      { .tick = 71, .actions = 3, .aim = { 1.0F, 2.0F }, .analog = 0.5F },
      { .tick = 73, .actions = U64_MAX, .aim = { -9.0F, 2.0F }, .analog = 0.5F },
      { .tick = 74, .actions = 0, .aim = { -9.0F, 2.0F }, .analog = 1.0F },
    };

    NYA_String* commands = nya_string_create(arena);
    NYA_EXPECT(nya_net_command_encode(commands, run, NYA_NET_COMMAND_REDUNDANCY));

    NYA_Object* object = nya_object_create(arena);
    nya_object_set(object, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "chat" });
    nya_object_set(object, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "hello there" });
    nya_object_set(object, "sender", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 3 });
    nya_object_set(object, "system", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false });

    NYA_String* document = nya_string_create(arena);
    NYA_EXPECT(nya_net_message_write_object(arena, document, object));

    u32 commands_ok = 0;
    u32 objects_ok  = 0;

    for (u32 iteration = 0; iteration < 30000; iteration++) {
      NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
      u32            count                               = 0;

      nya_memcpy(input, commands->items, commands->length);
      u64 size = mutate(input, commands->length, FUZZ_INPUT_MAX);

      if (nya_net_command_decode(input, size, received, &count).ok) {
        commands_ok++;
        nya_assert(count <= NYA_NET_COMMAND_REDUNDANCY);
        for (u32 i = 1; i < count; i++) nya_assert(received[i].tick > received[i - 1].tick, "a decoded run out of order");
      }

      nya_memcpy(input, document->items, document->length);
      size = mutate(input, document->length, FUZZ_INPUT_MAX);

      NYA_Arena*  scratch = nya_arena_create(.name = "fuzz_object");
      NYA_Object* parsed  = nullptr;

      if (nya_net_message_read_object(scratch, input, size, &parsed).ok && parsed != nullptr) {
        objects_ok++;

        // what a parsed event meets next: the chat layer, which trusts nothing in it.
        (void)nya_net_chat_client_consume(parsed);
      }

      u64 body = 0;
      nya_assert(nya_net_message_kind(input, size, &body) <= NYA_NET_MSG_COUNT && body <= size);

      nya_arena_destroy(scratch);
    }

    printf("  30000 mutated command runs (%u decoded) and documents (%u parsed)\n", commands_ok, objects_ok);
  }

  printf("TEST: the udp handshake and sealed packet parsers\n");
  {
    NYA_NetTransport* server = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));

    // only so the transport has a socket; nothing here sends to it, so its number is nobody's business.
    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    NET_Address* address = NET_ResolveHostname("127.0.0.1");
    nya_assert(address != nullptr && NET_WaitUntilResolved(address, 3000) == 1);

    _NYA_NetUdpState* state = server->state;

    u32 slot     = NYA_NET_MAX_PEERS;
    u32 dropped  = 0;
    u64 sequence = 1;

    for (u32 iteration = 0; iteration < 20000; iteration++) {
      // a peer with known keys, so sealed noise gets past the tag and into the fragment parser. Rejoined whenever the
      // noise gets it dropped, which is the transport doing its job.
      if (slot >= NYA_NET_MAX_PEERS || !state->peers[slot].occupied) {
        if (slot < NYA_NET_MAX_PEERS) dropped++;

        slot = _nya_net_udp_add_peer(state, address, 9);
        nya_assert(slot < NYA_NET_MAX_PEERS);

        state->peers[slot].established = true;
        for (u32 i = 0; i < NYA_NET_KEY_SIZE; i++) state->peers[slot].receive_key[i] = (u8)(i * 7);

        sequence = 1;
      }

      _NYA_NetUdpPeer* peer = &state->peers[slot];

      // handshake shaped noise from a stranger and from the peer's own address.
      u64 size = 5 + below(120);
      for (u64 i = 0; i < size; i++) input[i] = (u8)roll();
      _nya_net_udp_write_u32(input, 0x6E796106U);
      input[4] = (u8)below(8);

      _nya_net_udp_handle_handshake(server, (iteration & 1) ? slot : NYA_NET_MAX_PEERS, address, 9, input, size);

      // a sealed packet whose plaintext is noise shaped like fragments.
      u64 body = below(NYA_NET_MAX_DATAGRAM - 28);
      for (u64 i = 0; i < body; i++) input[12 + i] = (u8)roll();

      for (u64 at = 0; at + 9 <= body && below(2) == 0; at += 9 + below(40)) {
        input[12 + at] = (u8)below(3);
        _nya_net_udp_write_u16(input + 12 + at + 3, (u16)below(4));
        _nya_net_udp_write_u16(input + 12 + at + 5, (u16)(1 + below(4)));
        _nya_net_udp_write_u16(input + 12 + at + 7, (u16)below(64));
      }

      input[0] = below(8) == 0 ? 3 : 0;
      _nya_net_udp_write_u16(input + 1, (u16)sequence);
      _nya_net_udp_write_u16(input + 3, (u16)roll());
      _nya_net_udp_write_u32(input + 5, (u32)roll());
      _nya_net_udp_write_u16(input + 9, (u16)roll());
      input[11] = (u8)below(256);

      _nya_net_crypto_seal(peer->receive_key, sequence, input, 12, input + 12, body, input + 12 + body);
      sequence += 1 + below(3);

      _nya_net_udp_handle_packet(server, slot, input, 12 + body + 16);

      // every so often the peer's own garbage, which fails the tag.
      if (iteration % 7 == 0) _nya_net_udp_handle_packet(server, slot, input, mutate(input, 12 + body + 16, FUZZ_INPUT_MAX));

      if (iteration % 64 == 0) {
        NYA_NetTransportEvent event = { 0 };
        while (nya_net_transport_poll(server, &event)) { }
      }
    }

    printf("  20000 handshake and sealed packets parsed; the peer was dropped for misbehaving %u times\n", dropped);

    NET_UnrefAddress(address);
    nya_net_transport_destroy(server);
  }

  printf("TEST: what a client makes of a hostile server\n");
  {
    u32 kinds[] = { NYA_NET_MSG_WELCOME, NYA_NET_MSG_REJECT, NYA_NET_MSG_SNAPSHOT, NYA_NET_MSG_PEER_JOINED, NYA_NET_MSG_PEER_LEFT, NYA_NET_MSG_GAME_EVENT };

    for (u32 iteration = 0; iteration < 3000; iteration++) {
      NYA_NetTransport* server_end = nullptr;
      NYA_NetTransport* client_end = nullptr;
      NYA_EXPECT(nya_net_transport_loopback_create(arena, &server_end, &client_end));

      NYA_EXPECT(nya_net_client_attach(client_end, "victim", (NYA_NetClientConfig){
        .replicated_flag = FLAG_REPLICATED, .on_apply_command = nya_callback(apply_command), .on_sample_command = nya_callback(sample_command),
      }));

      // playing, so snapshots are decoded rather than ignored.
      _NYA_NET_CLIENT.state = NYA_NET_CLIENT_PLAYING;

      u64 size = 1 + below(300);
      input[0] = (u8)kinds[below(sizeof(kinds) / sizeof(kinds[0]))];
      for (u64 i = 1; i < size; i++) input[i] = (u8)roll();

      _nya_net_client_handle_message(input, size, 1.0F / 60.0F);

      nya_net_client_disconnect();
      nya_net_transport_destroy(server_end);
      nya_net_transport_destroy(client_end);
    }

    // restored: a WELCOME is allowed to change the tick.
    _NYA_APP_INSTANCE.options.time_step_ns = nya_time_ms_to_ns(16);

    printf("  3000 hostile server messages survived\n");
  }

  printf("TEST: what a server makes of a hostile joined client\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag = FLAG_REPLICATED, .on_spawn_player = nya_callback(spawn_player), .on_apply_command = nya_callback(apply_command), .max_speed = 10.0F,
    }));

    u64 tick = 1;

    for (u32 iteration = 0; iteration < 3000; iteration++) {
      if (nya_net_server_peer_count() == 0) {
        nya_net_server_stop();
        NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
          .replicated_flag = FLAG_REPLICATED, .on_spawn_player = nya_callback(spawn_player), .on_apply_command = nya_callback(apply_command), .max_speed = 10.0F,
        }));

        NYA_NetTransport* hostile = nullptr;
        NYA_EXPECT(nya_net_server_attach_local(&hostile));

        NYA_String* hello = nya_string_create(arena);
        nya_net_message_begin(hello, NYA_NET_MSG_HELLO);

        NYA_Object* body = nya_object_create(arena);
        nya_object_set(body, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
        nya_object_set(body, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
        nya_object_set(body, "tick", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = tick });
        NYA_EXPECT(nya_net_message_write_object(arena, hello, body));

        (void)nya_net_transport_send(hostile, (NYA_NetPeerId){ .index = 0, .generation = 1 }, NYA_NET_CHANNEL_RELIABLE, hello->items, hello->length);
        nya_net_server_tick(tick++, 1.0F / 60.0F);
      }

      // a command run with its acknowledgement in front, mutated.
      NYA_NetCommand command = { .tick = tick, .actions = roll(), .analog = (f32)(roll() % 1000) };

      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, (NYA_NetMessageKind)(iteration % 3 == 0 ? NYA_NET_MSG_GAME_EVENT : NYA_NET_MSG_COMMAND));
      _nya_net_write_varint(payload, tick - 1);
      NYA_EXPECT(nya_net_command_encode(payload, &command, 1));

      nya_memcpy(input, payload->items, payload->length);
      u64 size = mutate(input, payload->length, FUZZ_INPUT_MAX);

      _nya_net_server_handle_message(_NYA_NET_SERVER.loopback_server_end, nya_net_server_local_peer(), input, size, tick);
      nya_net_server_tick(tick++, 1.0F / 60.0F);
      nya_system_sim_apply_commands();
    }

    nya_net_server_stop();

    printf("  3000 mutated client messages survived\n");
  }

  NET_Quit();

  printf("PASSED: test_fuzz (0 failures)\n");

  return EXIT_SUCCESS;
}
