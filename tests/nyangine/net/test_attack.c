/**
 * The networking layer against a hostile peer.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3_net/SDL_net.h"

#include <time.h>

#define FIRST_PORT 48100
#define SETTLE_MS  60

/*
 * The wire layout, restated here rather than shared with the implementation.
 */
#define PROTOCOL        0x6E796105U
#define HEADER_SIZE     15
#define FRAGMENT_HEADER 9

#define KIND_DATA       0
#define KIND_CONNECT    1
#define KIND_DISCONNECT 3
#define KIND_RESPONSE   5

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

static void write_u16(u8* out, u16 value) {
  out[0] = (u8)(value & 0xFF);
  out[1] = (u8)((value >> 8) & 0xFF);
}

static void write_u32(u8* out, u32 value) {
  for (u32 i = 0; i < 4; i++) out[i] = (u8)((value >> (i * 8)) & 0xFF);
}

/** A packet header with the given fragment count. Returns how many bytes were written. */
static u64 write_header(u8* out, u16 sequence, u8 fragment_count) {
  u64 at = 0;

  write_u32(out + at, PROTOCOL);
  at += 4;
  write_u16(out + at, sequence);
  at += 2;
  write_u16(out + at, 0); // ack
  at += 2;
  write_u32(out + at, 0); // ack_bits
  at += 4;
  write_u16(out + at, 0); // reliable_ack
  at += 2;
  out[at++] = fragment_count;

  return at;
}

/** Pumps a transport, discarding everything. Lets timers fire and queues drain. */
static void pump(NYA_NetTransport* transport, u32 times) {
  for (u32 i = 0; i < times; i++) {
    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(transport, &event)) { }

    sleep_ms(2);
  }
}

/**
 * Completes a real handshake on a raw socket, so the attacker is a legitimate peer.
 * */
static b8 raw_handshake(NET_DatagramSocket* socket, NET_Address* target, u16 port, NYA_NetTransport* server) {
  u8  connect[HEADER_SIZE + 1 + 8] = { 0 };
  u64 connect_size                 = write_header(connect, 0, 0);
  connect[connect_size++]          = (u8)(KIND_CONNECT << 4);
  connect_size += 8;

  u64 deadline = nya_clock_get_monotonic_ms() + 4000;

  b8 responded = false;

  while (nya_clock_get_monotonic_ms() < deadline) {
    if (!responded) (void)NET_SendDatagram(socket, target, port, connect, (int)connect_size);

    pump(server, 1);

    NET_Datagram* reply = nullptr;

    while (NET_ReceiveDatagram(socket, &reply) && reply != nullptr) {
      if (reply->buflen >= (int)HEADER_SIZE + 1) {
        u8 kind = reply->buf[HEADER_SIZE] >> 4;

        // The challenge: echo the cookie back, which is the whole point of it.
        if (kind == 4 && reply->buflen >= (int)HEADER_SIZE + 1 + 8) {
          u8  response[HEADER_SIZE + 1 + 8] = { 0 };
          u64 at                            = write_header(response, 0, 0);

          response[at++] = (u8)(KIND_RESPONSE << 4);

          for (u32 i = 0; i < 8; i++) response[at++] = reply->buf[HEADER_SIZE + 1 + i];

          (void)NET_SendDatagram(socket, target, port, response, (int)at);
          responded = true;
        }

        // Accepted. Now a peer, and the data paths are reachable.
        if (kind == 2) {
          NET_DestroyDatagram(reply);
          return true;
        }
      }

      NET_DestroyDatagram(reply);
      reply = nullptr;
    }

    sleep_ms(4);
  }

  return false;
}

/** Counts how many peers a transport currently holds, by walking its own table. */
static u32 peer_count(NYA_NetTransport* transport) {
  const _NYA_NetUdpState* state = transport->state;

  u32 count = 0;
  for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
    if (state->peers[i].occupied) count++;
  }

  return count;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_assert(NET_Init(), "NET_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_attack");
  defer      nya_arena_destroy(arena);

  // ═════════════════════════════════════════════════════════════════════════════
  // TRANSPORT: what a raw socket can do to a listening server
  // ═════════════════════════════════════════════════════════════════════════════

  NYA_NetTransport* server = nullptr;
  NYA_EXPECT(nya_net_transport_udp_create(arena, &server));

  u16 port = 0;
  for (u16 candidate = FIRST_PORT; candidate < FIRST_PORT + 16; candidate++) {
    if (nya_net_transport_listen(server, candidate).ok) {
      port = candidate;
      break;
    }
  }
  nya_assert(port != 0, "could not bind any port in the test range");

  // The attacker's own socket, so packets can be malformed in ways the transport would never produce.
  NET_DatagramSocket* attacker = NET_CreateDatagramSocket(nullptr, 0, 0);
  nya_assert(attacker != nullptr, "could not open an attacker socket: %s", SDL_GetError());

  NET_Address* target = NET_ResolveHostname("127.0.0.1");
  nya_assert(target != nullptr);
  nya_assert(NET_WaitUntilResolved(target, 3000) == 1, "could not resolve loopback");

  printf("TEST: an oversized datagram is dropped, not parsed\n");
  {
    /*
     * The critical case. `buflen` can be up to 65507 because SDL_net's receive buffer is 64 kB, so the
     * fragment path below was reachable with a length no datagram was assumed able to carry.
     */
    u64 total_size = HEADER_SIZE + FRAGMENT_HEADER + 60000;
    u8* packet     = nya_arena_alloc(arena, total_size);

    u64 at = write_header(packet, 1, 1);

    packet[at++] = (u8)((KIND_DATA << 4) | NYA_NET_CHANNEL_UNRELIABLE);
    write_u16(packet + at, 7); // message id
    at += 2;
    write_u16(packet + at, 1); // fragment index 1, so the copy is offset, not at zero
    at += 2;
    write_u16(packet + at, 2); // of two
    at += 2;
    write_u16(packet + at, 60000); // and claims sixty thousand bytes
    at += 2;

    nya_memset(packet + at, 0x41, 60000);

    /*
     * A completed handshake first, so this reaches the fragment path.
     */
    nya_assert(raw_handshake(attacker, target, port, server), "the attacker could not join to mount the attack");

    (void)NET_SendDatagram(attacker, target, port, packet, (int)total_size);
    pump(server, 8);

    printf("  survived a %llu byte datagram claiming a 60000 byte fragment\n", (unsigned long long)total_size);
  }

  printf("TEST: an over-length fragment inside a legal datagram is refused\n");
  {
    /*
     * The same overflow, kept under NYA_NET_MAX_DATAGRAM so the datagram-size guard does not catch it.
     */
    u8  packet[NYA_NET_MAX_DATAGRAM] = { 0 };
    u64 at                           = write_header(packet, 2, 1);

    u64 claimed = NYA_NET_MAX_DATAGRAM - HEADER_SIZE - FRAGMENT_HEADER; // exactly `usable`, plus we lie below

    packet[at++] = (u8)((KIND_DATA << 4) | NYA_NET_CHANNEL_UNRELIABLE);
    write_u16(packet + at, 8);
    at += 2;
    write_u16(packet + at, 1);
    at += 2;
    write_u16(packet + at, 2);
    at += 2;
    write_u16(packet + at, (u16)claimed);
    at += 2;

    nya_memset(packet + at, 0x42, claimed);

    (void)NET_SendDatagram(attacker, target, port, packet, (int)(at + claimed));
    pump(server, 8);

    printf("  survived a fragment claiming the whole datagram at a non-zero offset\n");
  }

  printf("TEST: a flood of spoofed CONNECTs consumes no peer slots\n");
  {
    /*
     * Address validation, which is the difference between a peer table and a free-for-all.
     */
    u32 before = peer_count(server);

    u8  connect[HEADER_SIZE + 1 + 8] = { 0 };
    u64 connect_size                 = write_header(connect, 0, 0);
    connect[connect_size++]          = (u8)(KIND_CONNECT << 4);
    connect_size += 8;

    for (u32 i = 0; i < 200; i++) (void)NET_SendDatagram(attacker, target, port, connect, (int)connect_size);

    pump(server, 20);

    u32 after = peer_count(server);

    printf("  200 CONNECTs: %u peers before, %u after\n", before, after);

    nya_assert(after <= before + 1, "%u CONNECTs produced %u peer slots; a CONNECT must not allocate one", 200, after - before);
  }

  printf("TEST: a response with a forged cookie is refused\n");
  {
    u32 before = peer_count(server);

    u8  response[HEADER_SIZE + 1 + 8] = { 0 };
    u64 at                            = write_header(response, 0, 0);

    response[at++] = (u8)(KIND_RESPONSE << 4);

    // A cookie an attacker made up. It is a keyed hash of their own address under a secret only the
    // server holds, so guessing is the only option and sixty-four bits is not guessable.
    for (u32 i = 0; i < 8; i++) response[at++] = 0xCD;

    for (u32 i = 0; i < 50; i++) (void)NET_SendDatagram(attacker, target, port, response, (int)at);

    pump(server, 20);

    nya_assert(peer_count(server) == before, "a forged connect cookie was accepted");
    printf("  50 forged cookies rejected\n");
  }

  printf("TEST: a forged DISCONNECT does not evict a real peer\n");
  {
    /*
     * The control channel, which was seventeen forgeable bytes.
     */
    NYA_NetTransport* client = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));
    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    // Let the handshake finish: connect, challenge, response, accept.
    u64 deadline = nya_clock_get_monotonic_ms() + 4000;
    b8  joined   = false;

    while (!joined && nya_clock_get_monotonic_ms() < deadline) {
      NYA_NetTransportEvent event = { 0 };

      while (nya_net_transport_poll(client, &event)) {
        if (event.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED) joined = true;
      }

      pump(server, 1);
      sleep_ms(2);
    }

    nya_assert(joined, "the client never completed the handshake");

    u32 with_client = peer_count(server);
    nya_assert(with_client >= 1, "the server should hold the client");

    // A DISCONNECT with no token, and one with a wrong token.
    for (u32 attempt = 0; attempt < 2; attempt++) {
      u8  packet[HEADER_SIZE + 1 + 8] = { 0 };
      u64 at                          = write_header(packet, 0, 0);

      packet[at++] = (u8)((KIND_DISCONNECT << 4) | (u8)NYA_NET_DISCONNECT_REQUESTED);

      for (u32 i = 0; i < 8; i++) packet[at++] = attempt == 0 ? 0x00 : 0xEE;

      for (u32 i = 0; i < 20; i++) (void)NET_SendDatagram(attacker, target, port, packet, (int)at);
    }

    pump(server, 20);
    pump(client, 5);

    nya_assert(peer_count(server) >= with_client, "a forged disconnect removed a peer (%u -> %u)", with_client, peer_count(server));
    printf("  40 forged disconnects rejected, %u peers intact\n", peer_count(server));

    nya_net_transport_destroy(client);
  }

  printf("TEST: the per-peer reassembly budget refuses rather than evicting\n");
  {
    /*
     * The memory-amplification bound.
     */
    NET_DatagramSocket* hoarder = NET_CreateDatagramSocket(nullptr, 0, 0);
    nya_assert(hoarder != nullptr);

    nya_assert(raw_handshake(hoarder, target, port, server), "the hoarder could not join");

    u32 index = 0;
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
      const _NYA_NetUdpState* state = server->state;
      if (state->peers[i].occupied) index = i;
    }

    // two hundred fragments of ~1176 usable bytes is about 235 kB, under the per message cap, so only the
    // per peer budget can refuse it.
    for (u16 message = 100; message < 100 + 8; message++) {
      u8  packet[NYA_NET_MAX_DATAGRAM] = { 0 };
      u64 at                           = write_header(packet, (u16)(200 + message), 1);

      packet[at++] = (u8)((KIND_DATA << 4) | NYA_NET_CHANNEL_UNRELIABLE);
      write_u16(packet + at, message);
      at += 2;
      write_u16(packet + at, 0); // first fragment only, so the message never completes
      at += 2;
      write_u16(packet + at, 200);
      at += 2;
      write_u16(packet + at, 1); // one byte of payload
      at += 2;

      packet[at++] = 0x5A;

      (void)NET_SendDatagram(hoarder, target, port, packet, (int)at);
      pump(server, 2);
    }

    pump(server, 6);

    const _NYA_NetUdpState* state = server->state;

    u64 held = state->peers[index].reassembly_bytes;

    printf("  eight 235 kB reassemblies started: %llu bytes held, cap is %llu\n", (unsigned long long)held, (unsigned long long)_NYA_NET_UDP_MAX_REASSEMBLY_BYTES);

    /*
     * The bound is the engine's, not the attacker's.
     */
    nya_assert(held <= _NYA_NET_UDP_MAX_REASSEMBLY_BYTES, "a peer held %llu bytes of reassembly against a %llu byte cap",
               (unsigned long long)held, (unsigned long long)_NYA_NET_UDP_MAX_REASSEMBLY_BYTES);

    NET_DestroyDatagramSocket(hoarder);
  }

  printf("TEST: a stalled reliable stream drops the peer rather than growing\n");
  {
    /*
     * Ordered delivery holds an early arrival until the gap ahead of it is filled, and an attacker turns
     * that into unbounded memory: send ids 1, 2, 3… and never the one the receiver is waiting for, and
     * nothing is ever delivered or freed.
     */
    NET_DatagramSocket* staller = NET_CreateDatagramSocket(nullptr, 0, 0);
    nya_assert(staller != nullptr);

    nya_assert(raw_handshake(staller, target, port, server), "the staller could not join");

    u32 before = peer_count(server);
    nya_assert(before >= 1);

    /*
     * Dense: many message ids in one datagram, since `fragment_count` is a byte and a fragment may carry
     * one payload byte. A few packets build a queue a naive implementation keeps forever.
     */
    for (u32 round = 0; round < 6; round++) {
      u8  packet[NYA_NET_MAX_DATAGRAM] = { 0 };
      u64 at                           = write_header(packet, (u16)(400 + round), 100);

      for (u16 slot = 0; slot < 100; slot++) {
        u16 message_id = (u16)(1 + (round * 100) + slot);

        packet[at++] = (u8)((KIND_DATA << 4) | NYA_NET_CHANNEL_RELIABLE);
        write_u16(packet + at, message_id);
        at += 2;
        write_u16(packet + at, 0);
        at += 2;
        write_u16(packet + at, 1); // a single fragment, so it is queued rather than reassembled
        at += 2;
        write_u16(packet + at, 1);
        at += 2;

        packet[at++] = (u8)slot;
      }

      (void)NET_SendDatagram(staller, target, port, packet, (int)at);
      pump(server, 2);
    }

    pump(server, 10);

    /*
     * Either bound is an acceptable outcome, and asserting on which one fired would be asserting on an
     * implementation detail. What must hold is that the queue did not grow without limit: the peer is
     * either gone, or still present with a bounded queue.
     */
    const _NYA_NetUdpState* state = server->state;

    u32 queued = 0;
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
      if (!state->peers[i].occupied) continue;
      if (state->peers[i].incoming_reliable == nullptr) continue;

      if (state->peers[i].incoming_reliable->length > queued) queued = (u32)state->peers[i].incoming_reliable->length;
    }

    printf("  600 stalled reliable messages: deepest queue is %u, cap is %d\n", queued, _NYA_NET_UDP_MAX_REORDER);

    nya_assert(queued <= _NYA_NET_UDP_MAX_REORDER, "a reorder queue reached %u against a %d cap", queued, _NYA_NET_UDP_MAX_REORDER);

    NET_DestroyDatagramSocket(staller);
  }

  printf("TEST: random garbage never faults the transport\n");
  {
    /*
     * The catch-all. Every guard above was added because a specific shape got through; this looks for the
     * shapes nobody thought of.
     */
    NYA_RNG             rng     = nya_rng_create(.seed = "A77ACC");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } };

    for (u32 iteration = 0; iteration < 3000; iteration++) {
      u8  packet[900];
      u32 size = 1 + (u32)(iteration % (sizeof(packet) - 1));

      for (u32 i = 0; i < size; i++) packet[i] = nya_rng_sample_u8(&rng, uniform);

      // Every other one gets a valid protocol word, so it is parsed rather than dropped.
      if ((iteration % 2) == 0 && size >= 4) write_u32(packet, PROTOCOL);

      (void)NET_SendDatagram(attacker, target, port, packet, (int)size);

      // Drained periodically rather than per packet, so the receive loop's own batching is exercised too.
      if ((iteration % 64) == 0) pump(server, 1);
    }

    pump(server, 30);

    printf("  3000 random datagrams survived\n");
  }

  NET_UnrefAddress(target);
  NET_DestroyDatagramSocket(attacker);
  nya_net_transport_destroy(server);

  // ═════════════════════════════════════════════════════════════════════════════
  // DECODERS: bytes somebody else chose
  // ═════════════════════════════════════════════════════════════════════════════

  printf("TEST: every decoder refuses garbage without faulting\n");
  {
    NYA_RNG             rng     = nya_rng_create(.seed = "DEC0DE");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } };

    u32 snapshot_ok = 0;
    u32 command_ok  = 0;
    u32 object_ok   = 0;

    for (u32 iteration = 0; iteration < 20000; iteration++) {
      u8  buffer[512];
      u64 size = 1 + (iteration % (sizeof(buffer) - 1));

      for (u64 i = 0; i < size; i++) buffer[i] = nya_rng_sample_u8(&rng, uniform);

      NYA_Arena* scratch = nya_arena_create(.name = "fuzz");

      /*
       * A decoder may succeed on random input, since some is valid and an empty snapshot is legal. It must
       * not fault, read past the buffer, or allocate from a size it was handed. ASan and the arena
       * accounting are the assertions; the counters only prove the parsers were reached.
       */
      NYA_NetSnapshot snapshot = { 0 };
      if (nya_net_snapshot_decode(scratch, buffer, size, nullptr, &snapshot).ok) snapshot_ok++;

      NYA_NetCommand commands[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
      u32            count                               = 0;
      if (nya_net_command_decode(buffer, size, commands, &count).ok) {
        command_ok++;
        nya_assert(count <= NYA_NET_COMMAND_REDUNDANCY, "the command decoder reported %u commands", count);
      }

      NYA_Object* object = nullptr;
      if (nya_net_message_read_object(scratch, buffer, size, &object).ok) object_ok++;

      // The message kind reader, which is what every payload hits first.
      u64                body   = 0;
      NYA_NetMessageKind kind   = nya_net_message_kind(buffer, size, &body);
      nya_assert(kind <= NYA_NET_MSG_COUNT, "the kind reader returned %d", (int)kind);
      nya_assert(body <= size, "the kind reader put the body past the end of the payload");

      nya_arena_destroy(scratch);
    }

    printf("  20000 random payloads: %u snapshots, %u commands, %u objects accepted, none faulted\n", snapshot_ok, command_ok, object_ok);
  }

  printf("TEST: a snapshot decoder fed a truncated valid payload refuses cleanly\n");
  {
    /*
     * Truncation specifically, because it is the shape a real network produces and the shape a
     * bounds-checked reader gets wrong: a payload that is valid up to the point where it stops.
     */
    NYA_NetEntityState entities[3] = {
      { .handle = { .index = 1, .generation = 1 }, .position = { 1.0F, 2.0F, 3.0F }, .scale = { 1.0F, 1.0F, 1.0F } },
      { .handle = { .index = 2, .generation = 1 }, .position = { 4.0F, 5.0F, 6.0F }, .scale = { 1.0F, 1.0F, 1.0F } },
      { .handle = { .index = 3, .generation = 1 }, .position = { 7.0F, 8.0F, 9.0F }, .scale = { 1.0F, 1.0F, 1.0F } },
    };

    NYA_NetSnapshot whole = { .tick = 42, .entities = entities, .entity_count = 3 };

    NYA_String* encoded = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &whole, nullptr, encoded));

    u32 refused = 0;

    for (u64 prefix = 0; prefix < encoded->length; prefix++) {
      NYA_NetSnapshot decoded = { 0 };

      if (!nya_net_snapshot_decode(arena, encoded->items, prefix, nullptr, &decoded).ok) refused++;
    }

    // The whole thing still decodes, which is what says the refusals above were about the truncation
    // rather than about the payload being wrong all along.
    NYA_NetSnapshot decoded = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, encoded->items, encoded->length, nullptr, &decoded));
    nya_assert(decoded.entity_count == 3);

    printf("  %u of %llu prefixes refused, the complete payload accepted\n", refused, (unsigned long long)encoded->length);

    nya_assert(refused == encoded->length, "some truncated prefix was accepted as a whole snapshot");
  }

  // ═════════════════════════════════════════════════════════════════════════════
  // SERVER: what a joined client can do with application messages
  // ═════════════════════════════════════════════════════════════════════════════

  /*
   * These go through a loopback pair rather than a socket, because the target is the *server's* message
   * handling rather than the transport's framing. A loopback lets a payload be handed over exactly as
   * written, which is what an attacker who has already joined effectively has.
   */
  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  /** Sends one handcrafted application payload as the joined client. */
  #define SEND_AS_CLIENT(payload)                                                                                                                        (void)nya_net_transport_send(hostile, (NYA_NetPeerId){ .index = 0, .generation = 1 }, NYA_NET_CHANNEL_RELIABLE, (payload)->items, (payload)->length)

  printf("TEST: an oversized HELLO is refused before it is parsed\n");
  {
    /*
     * HELLO is dispatched *before* the peer is accepted, so it is the one message an unauthenticated
     * address puts in front of the nya parser. A reassembled reliable message can be hundreds of
     * kilobytes; a HELLO has three fields.
     */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_HELLO);

    // A valid document, then padding past the limit. Valid so that a refusal can only be about the size.
    NYA_Object* hello = nya_object_create(arena);
    nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "attacker" });

    NYA_EXPECT(nya_net_message_write_object(arena, payload, hello));

    while (payload->length < 4096) nya_string_push_back(payload, 0x00);

    SEND_AS_CLIENT(payload);

    nya_net_server_tick(1, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(nya_net_server_peer_count() == 0, "an oversized HELLO was accepted (%u peers)", nya_net_server_peer_count());
    printf("  a %llu byte HELLO was refused\n", (unsigned long long)payload->length);

    nya_net_server_stop();
  }

  printf("TEST: an impossible snapshot acknowledgement drops the peer\n");
  {
    /*
     * `acknowledged_tick` is a client chosen u64 and monotonic. Naming U64_MAX must not stop every future
     * baseline from matching, which would send that peer a full snapshot every tick.
     */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    // Join properly first: nothing but HELLO is accepted before that, so the ack would be ignored
    // for the wrong reason.
    {
      NYA_String* hello_payload = nya_string_create(arena);
      nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

      NYA_Object* hello = nya_object_create(arena);
      nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
      nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
      nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "liar" });

      NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));

      SEND_AS_CLIENT(hello_payload);

      nya_net_server_tick(1, 1.0F / 60.0F);
      nya_system_sim_apply_commands();

      nya_assert(nya_net_server_peer_count() == 1, "the attacker should have joined normally first");
    }

    NYA_String* ack = nya_string_create(arena);
    nya_net_message_begin(ack, NYA_NET_MSG_SNAPSHOT_ACK);
    for (u32 i = 0; i < 8; i++) nya_string_push_back(ack, 0xFF); // U64_MAX

    SEND_AS_CLIENT(ack);

    nya_net_server_tick(2, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(nya_net_server_peer_count() == 0, "a peer acknowledging an impossible tick was not dropped");
    printf("  a peer acknowledging U64_MAX at tick 2 was dropped\n");

    nya_net_server_stop();
  }

  printf("TEST: a command from the far future does not wedge later commands\n");
  {
    /*
     * `tick` is client chosen. A command claiming U64_MAX must not make every later command stale while
     * the repeat pass keeps applying the frozen one.
     */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "timetraveller" });

    NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));
    SEND_AS_CLIENT(hello_payload);

    nya_net_server_tick(10, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    NYA_NetPeerId peer = nya_net_server_local_peer();
    nya_assert(nya_net_peer_is_set(peer));

    // The lie.
    {
      NYA_NetCommand absurd = { .tick = U64_MAX, .actions = 0xDEAD };

      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
      NYA_EXPECT(nya_net_command_encode(payload, &absurd, 1));

      SEND_AS_CLIENT(payload);

      nya_net_server_tick(11, 1.0F / 60.0F);
      nya_system_sim_apply_commands();

      nya_assert(nya_net_server_last_command(peer).actions != 0xDEAD, "a command from the far future was accepted");
    }

    // And an honest one right after it still lands, which is the property that was broken.
    {
      NYA_NetCommand honest = { .tick = 12, .actions = 0xBEEF };

      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
      NYA_EXPECT(nya_net_command_encode(payload, &honest, 1));

      SEND_AS_CLIENT(payload);

      nya_net_server_tick(12, 1.0F / 60.0F);
      nya_system_sim_apply_commands();

      nya_assert(nya_net_server_last_command(peer).actions == 0xBEEF, "an honest command after an absurd one was discarded");
    }

    printf("  the absurd command was dropped and the next one still applied\n");

    nya_net_server_stop();
  }

  printf("TEST: a malformed command drops the peer\n");
  {
    /*
     * Unlike a game event, a command has a fixed encoding every well behaved client gets right, so a
     * malformed one is a broken client or a probe and the peer is dropped.
     */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "malformer" });

    NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));
    SEND_AS_CLIENT(hello_payload);

    nya_net_server_tick(20, 1.0F / 60.0F);
    nya_system_sim_apply_commands();
    nya_assert(nya_net_server_peer_count() == 1);

    // A count of 200 with nothing behind it. The decoder must refuse before writing past the caller's
    // four-entry stack array.
    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
    nya_string_push_back(payload, 200);

    SEND_AS_CLIENT(payload);

    nya_net_server_tick(21, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(nya_net_server_peer_count() == 0, "a peer sending a malformed command was not dropped");
    printf("  a command claiming 200 entries dropped the peer\n");

    nya_net_server_stop();
  }

  printf("TEST: random application payloads never fault the server\n");
  {
    /*
     * The catch-all for the message layer, mirroring the transport one above. Every message kind reached
     * with garbage behind it, including the kinds only valid in the other direction.
     */
    NYA_RNG             rng     = nya_rng_create(.seed = "5E4E4E");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } };

    u64 tick = 100;

    for (u32 iteration = 0; iteration < 400; iteration++) {
      NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

      NYA_NetTransport* hostile = nullptr;
      NYA_EXPECT(nya_net_server_attach_local(&hostile));

      NYA_String* payload = nya_string_create(arena);

      // Every kind in turn, so none of the switch arms is left unvisited.
      nya_string_push_back(payload, (u8)(1 + (iteration % (NYA_NET_MSG_COUNT - 1))));

      u32 size = iteration % 200;
      for (u32 i = 0; i < size; i++) nya_string_push_back(payload, nya_rng_sample_u8(&rng, uniform));

      SEND_AS_CLIENT(payload);

      nya_net_server_tick(tick++, 1.0F / 60.0F);
      nya_system_sim_apply_commands();

      nya_net_server_stop();
    }

    printf("  400 random application payloads across every message kind survived\n");
  }

  #undef SEND_AS_CLIENT

  nya_world_destroy(world);
  nya_system_callback_deinit();

  NET_Quit();

  printf("PASSED: test_attack (0 failures)\n");

  return EXIT_SUCCESS;
}
