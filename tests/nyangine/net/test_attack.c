/**
 * The networking layer against a hostile peer.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

/* The wire layout, restated here rather than shared with the implementation. */
#define PROTOCOL        0x6E796107U
#define HEADER_SIZE     12
#define FRAGMENT_HEADER 9
#define WHOLE_HEADER    5
#define WHOLE_FLAG      0x80
#define MAC_SIZE        16
#define KEY_SIZE        32

#define KIND_DATA       0
#define KIND_CONNECT    1
#define KIND_ACCEPT     2
#define KIND_DISCONNECT 3
#define KIND_CHALLENGE  4
#define KIND_RESPONSE   5
#define KIND_REFUSED    6

#define CONNECT_SIZE   64
#define CHALLENGE_SIZE 45
#define RESPONSE_SIZE  93
#define ACCEPT_SIZE    53

/* the largest datagram raw_send builds, under the 65507 bytes UDP carries over IPv4. */
#define RAW_PACKET_SIZE_MAX 65000

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

/** Pumps a transport, discarding everything. Lets timers fire and queues drain. */
static void pump(NYA_NetTransport* transport, u32 times) {
  for (u32 i = 0; i < times; i++) {
    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(transport, &event)) { }

    sleep_ms(2);
  }
}

/** A hostile client on its own socket that completed a real handshake, so its sealed packets are accepted. */
typedef struct {
  NYA_OsSocket socket;
  u8           send_key[KEY_SIZE];
  u8           receive_key[KEY_SIZE];
  u64          sequence;
} RawPeer;

static void send_connect(NYA_OsSocket socket, NYA_OsAddress target, u16 port, u32 size) {
  u8 connect[CONNECT_SIZE] = { 0 };
  write_u32(connect, PROTOCOL);
  connect[4] = KIND_CONNECT;

  target.port = port;

  (void)nya_os_socket_send_to(socket, target, connect, size);
}

/** The server's address with the port a case is aiming at: an address carries its port now. */
static NYA_OsAddress _target_at(NYA_OsAddress target, u16 port) {
  target.port = port;
  return target;
}

/**
 * Waits for a datagram of kind `kind` on `socket`, pumping the server meanwhile. Copies it into `out`, which holds
 * at least NYA_NET_MAX_DATAGRAM bytes, and returns its size, or zero on a timeout.
 * */
static u64 await_kind(NYA_OsSocket socket, NYA_NetTransport* server, u8 kind, u8* out, u32 timeout_ms) {
  u64 deadline = nya_clock_get_monotonic_ms() + timeout_ms;

  while (nya_clock_get_monotonic_ms() < deadline) {
    pump(server, 1);

    u8            reply[NYA_NET_MAX_DATAGRAM] = { 0 };
    u64           size                        = 0;
    NYA_OsAddress from                        = { 0 };

    while (nya_os_socket_receive_from(socket, reply, sizeof(reply), &size, &from) == NYA_OS_SOCKET_OK) {
      if (size >= 5 && reply[4] == kind) {
        nya_memcpy(out, reply, size);
        return size;
      }
    }

    sleep_ms(2);
  }

  return 0;
}

/** Builds a RESPONSE to `challenge` with a fresh ephemeral key, keeping what the ACCEPT will need. */
static void build_response(const u8* challenge, u8* response, NYA_NetKeyPair* ephemeral, u8* premaster) {
  NYA_EXPECT(nya_net_key_pair_create(ephemeral));

  u8 dh[KEY_SIZE]   = { 0 };
  u8 none[KEY_SIZE] = { 0 };
  u8 key[KEY_SIZE]  = { 0 };

  nya_assert(_nya_net_crypto_exchange(dh, ephemeral->secret_key, challenge + 13));

  write_u32(response, PROTOCOL);
  response[4] = KIND_RESPONSE;
  nya_memcpy(response + 5, challenge + 5, 8);
  nya_memcpy(response + 13, ephemeral->public_key, KEY_SIZE);
  nya_memset(response + 13 + KEY_SIZE, 0, KEY_SIZE);

  _nya_net_crypto_premaster(premaster, dh, none, challenge + 13, ephemeral->public_key, none);
  _nya_net_crypto_response_key(key, premaster);
  _nya_net_crypto_seal(key, 0, response, 13 + (KEY_SIZE * 2), nullptr, 0, response + 13 + (KEY_SIZE * 2));
}

/** Completes a real handshake on a raw socket, so the attacker is a legitimate, keyed peer. */
static b8 raw_handshake(RawPeer* peer, NYA_OsAddress target, u16 port, NYA_NetTransport* server) {
  u8 datagram[NYA_NET_MAX_DATAGRAM];

  for (u32 attempt = 0; attempt < 20; attempt++) {
    send_connect(peer->socket, target, port, CONNECT_SIZE);

    if (await_kind(peer->socket, server, KIND_CHALLENGE, datagram, 300) != CHALLENGE_SIZE) continue;

    u8             response[RESPONSE_SIZE];
    NYA_NetKeyPair ephemeral          = { 0 };
    u8             premaster[KEY_SIZE] = { 0 };

    build_response(datagram, response, &ephemeral, premaster);

    (void)nya_os_socket_send_to(peer->socket, _target_at(target, port), response, RESPONSE_SIZE);

    if (await_kind(peer->socket, server, KIND_ACCEPT, datagram, 300) != ACCEPT_SIZE) continue;

    u8 dh[KEY_SIZE] = { 0 };
    nya_assert(_nya_net_crypto_exchange(dh, ephemeral.secret_key, datagram + 5));

    _nya_net_crypto_session(peer->send_key, peer->receive_key, premaster, dh, datagram + 5);
    nya_assert(_nya_net_crypto_open(peer->receive_key, U64_MAX, datagram, 5 + KEY_SIZE, nullptr, 0, datagram + 5 + KEY_SIZE), "the ACCEPT did not verify");

    peer->sequence = 1;
    return true;
  }

  return false;
}

/** Seals `body` behind a header and sends it as the raw peer. `body` is encrypted in place. */
static void raw_send(RawPeer* peer, NYA_OsAddress target, u16 port, u8 kind, u8 fragment_count, u8* body, u64 body_size) {
  nya_assert(HEADER_SIZE + body_size + MAC_SIZE <= RAW_PACKET_SIZE_MAX);

  // static, not on the stack: 64 KB is a lot of frame, and the test sends from one thread only
  static u8 packet[RAW_PACKET_SIZE_MAX];
  u64       total = HEADER_SIZE + body_size + MAC_SIZE;

  u64 sequence = peer->sequence++;

  packet[0] = kind;
  write_u16(packet + 1, (u16)sequence);
  write_u16(packet + 3, 0);
  write_u32(packet + 5, 0);
  write_u16(packet + 9, 0);
  packet[11] = fragment_count;

  nya_memcpy(packet + HEADER_SIZE, body, body_size);
  _nya_net_crypto_seal(peer->send_key, sequence, packet, HEADER_SIZE, packet + HEADER_SIZE, body_size, packet + HEADER_SIZE + body_size);

  (void)nya_os_socket_send_to(peer->socket, _target_at(target, port), packet, total);
}

/** Writes one fragment header at `out`, returning its size. */
static u64 write_fragment(u8* out, u8 channel, u16 message_id, u16 index, u16 total, u16 length) {
  out[0] = channel;
  write_u16(out + 1, message_id);
  write_u16(out + 3, index);
  write_u16(out + 5, total);
  write_u16(out + 7, length);

  return FRAGMENT_HEADER;
}

/** Writes the short header of a message that fits one datagram at `out`, returning its size. */
static u64 write_whole(u8* out, u8 channel, u16 message_id, u16 length) {
  out[0] = channel | WHOLE_FLAG;
  write_u16(out + 1, message_id);
  write_u16(out + 3, length);

  return WHOLE_HEADER;
}

/** Pumps a transport like pump, counting the messages it delivers. */
static u32 pump_count(NYA_NetTransport* transport, u32 times) {
  u32 messages = 0;

  for (u32 i = 0; i < times; i++) {
    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(transport, &event)) messages += event.kind == NYA_NET_TRANSPORT_EVENT_MESSAGE ? 1 : 0;

    sleep_ms(2);
  }

  return messages;
}

/** What the cheating tests move: one entity, stepped by a fixed speed while action 0 is held. */
#define HONEST_SPEED 100.0F
#define CHEAT_TICK   (1.0F / 60.0F)

static NYA_EntityHandle CHEATER = NYA_ENTITY_HANDLE_NONE;

static NYA_EntityHandle spawn_cheater(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(peer, name);

  CHEATER = nya_entity_spawn(.name = "cheater", .flags = 1, .position = { 0.0F, 0.0F, 0.0F });
  return CHEATER;
}

static void walk(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  if (nya_net_command_holds(command, 0)) entity->position.x += HONEST_SPEED * delta_time_s;
}

/** A game that trusts what the command says about speed, which max_speed exists to contain. */
static void walk_trusting(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  entity->position.x += command->analog * delta_time_s;
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

/** The newest occupied slot, which after a handshake is the peer that just joined. */
static u32 newest_peer(NYA_NetTransport* transport) {
  const _NYA_NetUdpState* state = transport->state;

  u32 newest = 0;
  for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
    if (state->peers[i].occupied && state->peers[i].generation >= state->peers[newest].generation) newest = i;
  }

  return newest;
}

/**
 * Binds a fresh listening server on a port the system chose.
 *
 * Port zero, not a scan over a fixed window: two checkouts running their suites on one machine picked
 * the same window and collided on 48100, which is the failure this file was reported for.
 * */
static NYA_NetTransport* listen_server(NYA_Arena* arena, OUT u16* out_port) {
  NYA_NetTransport* server = nullptr;
  NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
  NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

  *out_port = nya_net_transport_port(server);

  return server;
}

/** A real client holding `identity` joins `port`, both ends pumped until the handshake ends one way or the other. The caller destroys the client. */
static NYA_NetTransport* join_as(NYA_Arena* arena, NYA_NetTransport* server, u16 port, NYA_NetKeyPair identity, OUT NYA_NetTransportEvent* out_outcome) {
  NYA_NetTransport* client = nullptr;
  NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ .identity = identity }, &client));
  NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

  *out_outcome = (NYA_NetTransportEvent){ 0 };
  u64 deadline = nya_clock_get_monotonic_ms() + 3000;

  while (out_outcome->kind == NYA_NET_TRANSPORT_EVENT_NONE && nya_clock_get_monotonic_ms() < deadline) {
    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(client, &event)) {
      if (event.kind != NYA_NET_TRANSPORT_EVENT_MESSAGE) *out_outcome = event;
    }

    pump(server, 1);
  }

  return client;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_assert(nya_os_socket_start() == NYA_OS_SOCKET_OK, "the host's socket library would not start");

  NYA_Arena* arena = nya_arena_create(.name = "test_attack");
  defer      nya_arena_destroy(arena);

  // ═════════════════════════════════════════════════════════════════════════════ TRANSPORT: what a raw socket can do to a listening server ═════════════════════════════════════════════════════════════════════════════

  u16               port   = 0;
  NYA_NetTransport* server = listen_server(arena, &port);

  NYA_OsAddress target = { 0 };
  nya_assert(nya_os_address_resolve("127.0.0.1", 0, NYA_OS_ADDRESS_V4, &target) == NYA_OS_SOCKET_OK, "could not resolve loopback");

  printf("TEST: a CONNECT is answered without amplification, and only at a bounded rate\n");
  {
    NYA_OsSocket flooder = NYA_OS_SOCKET_NONE;
    nya_assert(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &flooder) == NYA_OS_SOCKET_OK, "a raw socket would not open");

    u8 datagram[NYA_NET_MAX_DATAGRAM];

    // a short CONNECT would make the challenge larger than the request, so it gets nothing.
    send_connect(flooder, target, port, 16);
    nya_assert(await_kind(flooder, server, KIND_CHALLENGE, datagram, 150) == 0, "a short CONNECT was answered");

    send_connect(flooder, target, port, CONNECT_SIZE);
    u64 size = await_kind(flooder, server, KIND_CHALLENGE, datagram, 500);
    nya_assert(size == CHALLENGE_SIZE && size <= CONNECT_SIZE, "a CONNECT got a %llu byte answer", (unsigned long long)size);

    // the address's allowance, not the flood's size, decides how many answers go out.
    u32 before = peer_count(server);

    for (u32 i = 0; i < 400; i++) send_connect(flooder, target, port, CONNECT_SIZE);

    pump(server, 10);

    u32 answered = 0;
    while (await_kind(flooder, server, KIND_CHALLENGE, datagram, 50) != 0) answered++;

    printf("  400 CONNECTs from one address: %u challenges, %u new peers\n", answered, peer_count(server) - before);

    nya_assert(answered <= 24, "a flood of CONNECTs got %u answers from a rate limited server", answered);
    nya_assert(peer_count(server) == before, "a CONNECT took a peer slot");

    nya_os_socket_close(flooder);
    sleep_ms(2100);
  }

  printf("TEST: a forged cookie or a bad tag takes no slot\n");
  {
    NYA_OsSocket forger = NYA_OS_SOCKET_NONE;
    nya_assert(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &forger) == NYA_OS_SOCKET_OK, "a raw socket would not open");

    u32 before = peer_count(server);

    u8 datagram[NYA_NET_MAX_DATAGRAM];
    send_connect(forger, target, port, CONNECT_SIZE);
    nya_assert(await_kind(forger, server, KIND_CHALLENGE, datagram, 500) == CHALLENGE_SIZE);

    u8             response[RESPONSE_SIZE];
    NYA_NetKeyPair ephemeral          = { 0 };
    u8             premaster[KEY_SIZE] = { 0 };

    // a made up cookie: a keyed hash under a secret only the server holds, so guessing is the only option.
    build_response(datagram, response, &ephemeral, premaster);
    for (u32 i = 0; i < 8; i++) response[5 + i] ^= 0xCD;
    (void)nya_os_socket_send_to(forger, _target_at(target, port), response, RESPONSE_SIZE);

    // a real cookie with a tag that does not match: a client that does not know what the server's key needs.
    build_response(datagram, response, &ephemeral, premaster);
    response[RESPONSE_SIZE - 1] ^= 0x01;
    (void)nya_os_socket_send_to(forger, _target_at(target, port), response, RESPONSE_SIZE);

    // a low order ephemeral key, which would force an all zero shared secret.
    build_response(datagram, response, &ephemeral, premaster);
    nya_memset(response + 13, 0, KEY_SIZE);
    (void)nya_os_socket_send_to(forger, _target_at(target, port), response, RESPONSE_SIZE);

    nya_assert(await_kind(forger, server, KIND_ACCEPT, datagram, 400) == 0, "a forged response was accepted");
    nya_assert(peer_count(server) == before, "a forged response took a slot");

    printf("  forged cookie, bad tag and low order key refused\n");

    nya_os_socket_close(forger);
  }

  RawPeer attacker = { 0 };
  nya_assert(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &attacker.socket) == NYA_OS_SOCKET_OK, "a raw socket would not open");

  nya_assert(raw_handshake(&attacker, target, port, server), "the attacker could not join to mount the attacks");

  printf("TEST: an oversized datagram is dropped, not parsed\n");
  {
    /* `buflen` can be up to 65507 because SDL_net's receive buffer is 64 kB, so this is reachable. */
    u64 body_size = FRAGMENT_HEADER + 60000;
    u8* body      = nya_arena_alloc(arena, body_size);

    u64 at = write_fragment(body, NYA_NET_CHANNEL_UNRELIABLE, 7, 1, 2, 60000);
    nya_memset(body + at, 0x41, 60000);

    raw_send(&attacker, target, port, KIND_DATA, 1, body, body_size);
    pump(server, 8);

    printf("  survived a %llu byte sealed datagram claiming a 60000 byte fragment\n", (unsigned long long)(HEADER_SIZE + body_size + MAC_SIZE));
  }

  printf("TEST: an over-length fragment inside a legal sealed datagram is refused\n");
  {
    u8  body[NYA_NET_MAX_DATAGRAM - HEADER_SIZE - MAC_SIZE] = { 0 };
    u64 claimed                                            = sizeof(body) - FRAGMENT_HEADER;

    // index 1 of 2 claims a whole datagram's worth at a non-zero offset, one byte more than the body holds.
    u64 at = write_fragment(body, NYA_NET_CHANNEL_UNRELIABLE, 8, 1, 2, (u16)(claimed + 1));
    nya_memset(body + at, 0x42, claimed);

    raw_send(&attacker, target, port, KIND_DATA, 1, body, sizeof(body));

    // and a fragment count far past what the body holds.
    at = write_fragment(body, NYA_NET_CHANNEL_UNRELIABLE, 9, 0, 1, 4);
    raw_send(&attacker, target, port, KIND_DATA, 255, body, at + 4);

    pump(server, 8);

    nya_assert(peer_count(server) >= 1, "the server lost its peers to a malformed fragment");
    printf("  survived fragments claiming more than the datagram holds\n");
  }

  printf("TEST: a bundle is delivered whole or not at all\n");
  {
    const _NYA_NetUdpState* state = server->state;
    const _NYA_NetUdpPeer*  peer  = &state->peers[newest_peer(server)];

    // ids below the reassembly test's 100, so the duplicate window does not take those for old ones.
    u8  body[64] = { 0 };
    u64 good     = write_whole(body, NYA_NET_CHANNEL_UNRELIABLE, 20, 4) + 4;
    u64 pair     = good + write_whole(body + good, NYA_NET_CHANNEL_UNRELIABLE, 21, 4) + 4;

    // the control: two messages in one sealed packet both arrive, so the refusals below are not a peer that stopped listening.
    raw_send(&attacker, target, port, KIND_DATA, 2, body, pair);
    nya_assert_eq(pump_count(server, 4), 2U);

    u64 rejected = peer->stats.packets_rejected;

    // an id nothing has delivered, so each bundle below would hand its first message up if it were accepted.
    (void)write_whole(body, NYA_NET_CHANNEL_UNRELIABLE, 30, 4);
    nya_memset(body + good, 0, sizeof(body) - good);
    raw_send(&attacker, target, port, KIND_DATA, 2, body, good + 3);  // a second header cut short
    raw_send(&attacker, target, port, KIND_DATA, 2, body, good);      // a count past what the body holds
    raw_send(&attacker, target, port, KIND_DATA, 1, body, good + 1);  // a byte after the last message

    (void)write_whole(body + good, NYA_NET_CHANNEL_UNRELIABLE, 22, 200);
    raw_send(&attacker, target, port, KIND_DATA, 2, body, good + WHOLE_HEADER + 4);  // a length past the end

    body[0] |= 0x40;
    raw_send(&attacker, target, port, KIND_DATA, 1, body, good);  // a flag bit that means nothing

    u32 delivered = pump_count(server, 6);

    printf("  5 malformed bundles, each with a good message first: %u delivered, %llu rejected\n", delivered,
           (unsigned long long)(peer->stats.packets_rejected - rejected));

    nya_assert_eq(delivered, 0U);
    nya_assert_eq(peer->stats.packets_rejected - rejected, 5ULL);
    nya_assert(peer->occupied, "a malformed bundle cost the sender its connection rather than the packet");
  }

  printf("TEST: a forged, tampered or replayed packet is rejected before it is read\n");
  {
    NYA_NetTransport* client = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));
    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    u64 deadline = nya_clock_get_monotonic_ms() + 4000;
    b8  joined   = false;
    u32 messages = 0;

    NYA_NetPeerId to_server = NYA_NET_PEER_NONE;

    while (!joined && nya_clock_get_monotonic_ms() < deadline) {
      NYA_NetTransportEvent event = { 0 };

      while (nya_net_transport_poll(client, &event)) {
        if (event.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED) {
          joined    = true;
          to_server = event.peer;
        }
      }

      pump(server, 1);
    }

    nya_assert(joined, "the client never completed the handshake");

    u32 slot = newest_peer(server);

    _NYA_NetUdpState* server_state = server->state;
    _NYA_NetUdpState* client_state = client->state;

    u32 with_client = peer_count(server);

    // an unauthenticated DISCONNECT from the attacker's socket, in the old unsealed shape and as garbage.
    for (u32 attempt = 0; attempt < 20; attempt++) {
      u8 packet[HEADER_SIZE + 1 + 8] = { KIND_DISCONNECT };
      (void)nya_os_socket_send_to(attacker.socket, _target_at(target, port), packet, sizeof(packet));
    }

    // a message the server receives, whose sealed bytes are then replayed and tampered with as if from the client's address.
    u8 payload[24] = { 0x10 };
    NYA_EXPECT(nya_net_transport_send(client, to_server, NYA_NET_CHANNEL_UNRELIABLE, payload, sizeof(payload)));
    nya_net_transport_flush(client);

    u64 sealed_size = HEADER_SIZE + WHOLE_HEADER + sizeof(payload) + MAC_SIZE;
    u8  sealed[HEADER_SIZE + WHOLE_HEADER + sizeof(payload) + MAC_SIZE];
    nya_memcpy(sealed, client_state->send_buffer, sealed_size);

    deadline = nya_clock_get_monotonic_ms() + 2000;
    while (messages == 0 && nya_clock_get_monotonic_ms() < deadline) {
      NYA_NetTransportEvent event = { 0 };
      while (nya_net_transport_poll(server, &event)) {
        if (event.kind == NYA_NET_TRANSPORT_EVENT_MESSAGE) messages++;
      }
      sleep_ms(2);
    }

    nya_assert(messages == 1, "the genuine message did not arrive");

    u64 rejected_before = server_state->peers[slot].stats.packets_rejected;

    u8 replay[sizeof(sealed)];
    nya_memcpy(replay, sealed, sizeof(sealed));
    _nya_net_udp_handle_packet(server, slot, replay, sealed_size);

    u8 tampered[sizeof(sealed)];
    nya_memcpy(tampered, sealed, sizeof(sealed));
    write_u16(tampered + 1, 900);
    _nya_net_udp_handle_packet(server, slot, tampered, sealed_size);

    nya_memcpy(tampered, sealed, sizeof(sealed));
    write_u16(tampered + 1, 901);
    tampered[HEADER_SIZE + 3] ^= 0x80;
    _nya_net_udp_handle_packet(server, slot, tampered, sealed_size);

    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(server, &event)) {
      if (event.kind == NYA_NET_TRANSPORT_EVENT_MESSAGE) messages++;
    }

    u64 rejected = server_state->peers[slot].stats.packets_rejected - rejected_before;

    printf("  replay and two tamperings: %llu rejected, %u messages delivered in total\n", (unsigned long long)rejected, messages);

    nya_assert(messages == 1, "a replayed or tampered packet was delivered");
    nya_assert(rejected == 3, "only %llu of 3 bad packets were counted as rejected", (unsigned long long)rejected);
    nya_assert(peer_count(server) >= with_client, "a forged disconnect removed a peer (%u -> %u)", with_client, peer_count(server));

    nya_net_transport_destroy(client);
    pump(server, 4);
  }

  printf("TEST: the per-peer reassembly budget refuses rather than evicting\n");
  {
    u32 index = newest_peer(server);
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
      const _NYA_NetUdpState* state = server->state;
      if (state->peers[i].occupied && state->peers[i].established) index = i;
    }

    // two hundred fragments of ~1163 bytes is about 233 kB, under the per message cap, so only the per peer budget refuses.
    for (u16 message = 100; message < 100 + 8; message++) {
      u8  body[FRAGMENT_HEADER + 1];
      u64 at   = write_fragment(body, NYA_NET_CHANNEL_UNRELIABLE, message, 0, 200, 1);
      body[at] = 0x5A;

      raw_send(&attacker, target, port, KIND_DATA, 1, body, sizeof(body));
      pump(server, 2);
    }

    pump(server, 6);

    const _NYA_NetUdpState* state = server->state;

    u64 held = 0;
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) held = nya_max(held, state->peers[i].reassembly_bytes);

    printf("  eight 233 kB reassemblies started: %llu bytes held, cap is %llu\n", (unsigned long long)held, (unsigned long long)_NYA_NET_UDP_MAX_REASSEMBLY_BYTES);

    nya_assert(held > 0, "no reassembly was started, so the budget was never tested");
    nya_assert(held <= _NYA_NET_UDP_MAX_REASSEMBLY_BYTES, "a peer held %llu bytes of reassembly against a %llu byte cap", (unsigned long long)held,
               (unsigned long long)_NYA_NET_UDP_MAX_REASSEMBLY_BYTES);
    nya_unused(index);
  }

  printf("TEST: a stalled reliable stream drops the peer rather than growing\n");
  {
    /* Send ids 1, 2, 3... and never 0, the one the receiver waits for, and nothing is ever delivered or freed. */
    for (u32 round = 0; round < 6; round++) {
      u8  body[NYA_NET_MAX_DATAGRAM - HEADER_SIZE - MAC_SIZE];
      u64 at = 0;

      for (u16 slot = 0; slot < 100; slot++) {
        at += write_fragment(body + at, NYA_NET_CHANNEL_RELIABLE, (u16)(1 + (round * 100) + slot), 0, 1, 1);
        body[at++] = (u8)slot;
      }

      raw_send(&attacker, target, port, KIND_DATA, 100, body, at);
      pump(server, 2);
    }

    pump(server, 10);

    const _NYA_NetUdpState* state = server->state;

    u32 queued = 0;
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
      if (!state->peers[i].occupied || state->peers[i].incoming_reliable == nullptr) continue;

      queued = nya_max(queued, (u32)state->peers[i].incoming_reliable->length);
    }

    printf("  600 stalled reliable messages: deepest queue is %u, cap is %d\n", queued, _NYA_NET_UDP_MAX_REORDER);

    nya_assert(queued <= _NYA_NET_UDP_MAX_REORDER, "a reorder queue reached %u against a %d cap", queued, _NYA_NET_UDP_MAX_REORDER);
  }

  printf("TEST: one address cannot hold more than a few slots\n");
  {
    u16               cap_port   = 0;
    NYA_NetTransport* cap_server = listen_server(arena, &cap_port);

    RawPeer peers[_NYA_NET_UDP_MAX_PEERS_PER_ADDRESS + 1] = { 0 };

    for (u32 i = 0; i < _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS; i++) {
      nya_assert(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &peers[i].socket) == NYA_OS_SOCKET_OK, "a raw socket would not open");
      nya_assert(raw_handshake(&peers[i], target, cap_port, cap_server), "join %u of the allowed %d failed", i, _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS);
    }

    RawPeer* extra = &peers[_NYA_NET_UDP_MAX_PEERS_PER_ADDRESS];
    nya_assert(nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &extra->socket) == NYA_OS_SOCKET_OK, "a raw socket would not open");

    u8 datagram[NYA_NET_MAX_DATAGRAM];
    send_connect(extra->socket, target, cap_port, CONNECT_SIZE);
    nya_assert(await_kind(extra->socket, cap_server, KIND_CHALLENGE, datagram, 500) == CHALLENGE_SIZE);

    u8             response[RESPONSE_SIZE];
    NYA_NetKeyPair ephemeral          = { 0 };
    u8             premaster[KEY_SIZE] = { 0 };

    build_response(datagram, response, &ephemeral, premaster);
    (void)nya_os_socket_send_to(extra->socket, _target_at(target, cap_port), response, RESPONSE_SIZE);

    u64 refused = await_kind(extra->socket, cap_server, KIND_REFUSED, datagram, 500);

    printf("  %u peers from 127.0.0.1, the next one refused (%llu byte answer)\n", peer_count(cap_server), (unsigned long long)refused);

    nya_assert(peer_count(cap_server) == _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS, "one address holds %u slots", peer_count(cap_server));
    nya_assert(refused == 6 && datagram[5] == NYA_NET_DISCONNECT_FULL, "the extra connection was not refused as full");

    for (u32 i = 0; i <= _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS; i++) nya_os_socket_close(peers[i].socket);

    nya_net_transport_destroy(cap_server);
  }

  printf("TEST: an allowlist is open when zero, closed by its first key, and bounded\n");
  {
    NYA_NetKeyPair player = { 0 };
    NYA_NetKeyPair other  = { 0 };
    NYA_EXPECT(nya_net_key_pair_create(&player));
    NYA_EXPECT(nya_net_key_pair_create(&other));

    u8               zero[KEY_SIZE] = { 0 };
    NYA_NetAllowlist list           = { 0 };

    nya_assert(nya_net_allowlist_admits(&list, other.public_key) && nya_net_allowlist_admits(&list, nullptr), "a zero list is an open server");
    nya_assert(nya_net_allowlist_add(&list, zero).kind == NYA_ERROR_INVALID_ARGUMENT && !list.closed, "an all-zero key is not a key, and changes nothing");

    NYA_EXPECT(nya_net_allowlist_add(&list, player.public_key));
    NYA_EXPECT(nya_net_allowlist_add(&list, player.public_key));
    nya_assert_eq(list.count, 1U);
    nya_assert(list.closed && nya_net_allowlist_admits(&list, player.public_key), "the listed key is admitted");
    nya_assert(!nya_net_allowlist_admits(&list, other.public_key) && !nya_net_allowlist_admits(&list, zero), "the rest are not, keyless included");

    NYA_NetAllowlist full = { 0 };
    for (u32 i = 0; i < NYA_NET_ALLOWLIST_MAX; i++) {
      u8 key[KEY_SIZE] = { 0 };
      write_u32(key, i + 1);
      NYA_EXPECT(nya_net_allowlist_add(&full, key));
    }

    nya_assert(nya_net_allowlist_add(&full, other.public_key).kind == NYA_ERROR_OUT_OF_MEMORY && !nya_net_allowlist_contains(&full, other.public_key));

    // the last entry fills the hole a removal leaves, and nothing else moves.
    u8 first[KEY_SIZE] = { 0 };
    u8 last[KEY_SIZE]  = { 0 };
    write_u32(first, 1);
    write_u32(last, NYA_NET_ALLOWLIST_MAX);

    nya_net_allowlist_remove(&full, first);
    nya_net_allowlist_remove(&full, first);
    nya_assert_eq(full.count, (u32)NYA_NET_ALLOWLIST_MAX - 1);
    nya_assert(!nya_net_allowlist_contains(&full, first) && nya_net_allowlist_contains(&full, last) && nya_memcmp(full.keys[0], last, KEY_SIZE) == 0);
  }

  printf("TEST: a closed allowlist refuses a stranger and a keyless client at the handshake, with the reason\n");
  {
    NYA_NetKeyPair player   = { 0 };
    NYA_NetKeyPair stranger = { 0 };
    NYA_EXPECT(nya_net_key_pair_create(&player));
    NYA_EXPECT(nya_net_key_pair_create(&stranger));

    u16               allow_port   = 0;
    NYA_NetTransport* allow_server = listen_server(arena, &allow_port);
    NYA_EXPECT(nya_net_allowlist_add(&allow_server->allowlist, player.public_key));

    NYA_NetTransportEvent admitted_outcome = { 0 };
    NYA_NetTransport*     admitted         = join_as(arena, allow_server, allow_port, player, &admitted_outcome);
    nya_assert(admitted_outcome.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED, "the listed key did not connect");

    NYA_NetKeyPair refused_identities[] = { stranger, { 0 } };

    for (u32 i = 0; i < sizeof(refused_identities) / sizeof(refused_identities[0]); i++) {
      NYA_NetTransportEvent outcome = { 0 };
      NYA_NetTransport*     refused = join_as(arena, allow_server, allow_port, refused_identities[i], &outcome);

      nya_assert(outcome.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED, "client %u was not refused", i);
      nya_assert_eq((u32)outcome.reason, (u32)NYA_NET_DISCONNECT_IDENTITY);

      nya_net_transport_destroy(refused);
    }

    nya_assert_eq(peer_count(allow_server), 1U);

    // removal is checked at the next handshake only: the connected player stays, the same key joining again does not.
    nya_net_allowlist_remove(&allow_server->allowlist, player.public_key);
    nya_assert(allow_server->allowlist.closed, "removing the last key must not open the server");

    const u8 still_here[] = { 0x10, 'h', 'i' };
    NYA_EXPECT(nya_net_transport_send(admitted, admitted_outcome.peer, NYA_NET_CHANNEL_RELIABLE, still_here, sizeof(still_here)));
    nya_net_transport_flush(admitted);
    nya_assert(pump_count(allow_server, 50) == 1 && peer_count(allow_server) == 1, "a connected player was dropped by removing its key");

    NYA_NetTransportEvent again_outcome = { 0 };
    NYA_NetTransport*     again         = join_as(arena, allow_server, allow_port, player, &again_outcome);
    nya_assert(again_outcome.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED && again_outcome.reason == NYA_NET_DISCONNECT_IDENTITY, "a removed key joined again");

    printf("  the listed key joined, a stranger and a keyless client were refused, and a removal held off only the next join\n");

    nya_net_transport_destroy(again);
    nya_net_transport_destroy(admitted);
    nya_net_transport_destroy(allow_server);
  }

  printf("TEST: random garbage never faults the transport\n");
  {
    /* The catch-all. Every guard above was added because a specific shape got through; this looks for the shapes nobody thought of. */
    NYA_RNG             rng     = nya_rng_create(.seed = "A77ACC");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } };

    for (u32 iteration = 0; iteration < 3000; iteration++) {
      u8  packet[900];
      u32 size = 1 + (u32)(iteration % (sizeof(packet) - 1));

      for (u32 i = 0; i < size; i++) packet[i] = nya_rng_sample_u8(&rng, uniform);

      // every other one gets a valid protocol word, so the handshake parser sees it too.
      if ((iteration % 2) == 0 && size >= 4) write_u32(packet, PROTOCOL);

      (void)nya_os_socket_send_to(attacker.socket, _target_at(target, port), packet, size);

      // Drained periodically rather than per packet, so the receive loop's own batching is exercised too.
      if ((iteration % 64) == 0) pump(server, 1);
    }

    pump(server, 30);

    printf("  3000 random datagrams survived\n");
  }

  nya_os_socket_close(attacker.socket);
  nya_net_transport_destroy(server);

  // ═════════════════════════════════════════════════════════════════════════════ DECODERS: bytes somebody else chose ═════════════════════════════════════════════════════════════════════════════

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

      /* A decoder may succeed on random input, since some is valid and an empty snapshot is legal. It must not fault, read past the buffer, or allocate from a size it was handed. ASan and the arena accounting are the assertions; the counters only prove the parsers were reached. */
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
    /* Truncation specifically, because it is the shape a real network produces and the shape a bounds-checked reader gets wrong: a payload that is valid up to the point where it stops. */
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

    // The whole thing still decodes, which is what says the refusals above were about the truncation rather than about the payload being wrong all along.
    NYA_NetSnapshot decoded = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, encoded->items, encoded->length, nullptr, &decoded));
    nya_assert(decoded.entity_count == 3);

    printf("  %u of %llu prefixes refused, the complete payload accepted\n", refused, (unsigned long long)encoded->length);

    nya_assert(refused == encoded->length, "some truncated prefix was accepted as a whole snapshot");
  }

  // ═════════════════════════════════════════════════════════════════════════════ SERVER: what a joined client can do with application messages ═════════════════════════════════════════════════════════════════════════════

  /* These go through a loopback pair rather than a socket, because the target is the *server's* message handling rather than the transport's framing. A loopback lets a payload be handed over exactly as written, which is what an attacker who has already joined effectively has. */
  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  /** Sends one handcrafted application payload as the joined client. */
  #define SEND_AS_CLIENT(payload)                                                                                                                        (void)nya_net_transport_send(hostile, (NYA_NetPeerId){ .index = 0, .generation = 1 }, NYA_NET_CHANNEL_RELIABLE, (payload)->items, (payload)->length)

  printf("TEST: an oversized HELLO is refused before it is parsed\n");
  {
    /* HELLO is dispatched *before* the peer is accepted, so it is the one message an unauthenticated address puts in front of the nya parser. A reassembled reliable message can be hundreds of kilobytes; a HELLO has three fields. */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_HELLO);

    // A valid document, then padding past the limit. Valid so that a refusal can only be about the size.
    NYA_Object* hello = nya_object_create(arena);
    nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "attacker" });

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
    /* `acknowledged_tick` is a client chosen u64 and monotonic. Naming U64_MAX must not stop every future baseline from matching, which would send that peer a full snapshot every tick. */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    // Join properly first: nothing but HELLO is accepted before that, so the ack would be ignored for the wrong reason.
    {
      NYA_String* hello_payload = nya_string_create(arena);
      nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

      NYA_Object* hello = nya_object_create(arena);
      nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
      nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
      nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "liar" });

      NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));

      SEND_AS_CLIENT(hello_payload);

      nya_net_server_tick(1, 1.0F / 60.0F);
      nya_system_sim_apply_commands();

      nya_assert(nya_net_server_peer_count() == 1, "the attacker should have joined normally first");
    }

    // the acknowledgement rides at the front of every command.
    NYA_NetCommand honest = { .tick = 1 };

    NYA_String* ack = nya_string_create(arena);
    nya_net_message_begin(ack, NYA_NET_MSG_COMMAND);
    _nya_net_write_varint(ack, U64_MAX);
    NYA_EXPECT(nya_net_command_encode(ack, &honest, 1));

    SEND_AS_CLIENT(ack);

    nya_net_server_tick(2, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(nya_net_server_peer_count() == 0, "a peer acknowledging an impossible tick was not dropped");
    printf("  a peer acknowledging U64_MAX at tick 2 was dropped\n");

    nya_net_server_stop();
  }

  printf("TEST: a command from the far future does not wedge later commands\n");
  {
    /* `tick` is client chosen. A command claiming U64_MAX must not make every later command stale while the repeat pass keeps applying the frozen one. */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "timetraveller" });

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
      _nya_net_write_varint(payload, 0);
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
      _nya_net_write_varint(payload, 0);
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
    /* Unlike a game event, a command has a fixed encoding every well behaved client gets right, so a malformed one is a broken client or a probe and the peer is dropped. */
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1 }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "malformer" });

    NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));
    SEND_AS_CLIENT(hello_payload);

    nya_net_server_tick(20, 1.0F / 60.0F);
    nya_system_sim_apply_commands();
    nya_assert(nya_net_server_peer_count() == 1);

    // A count of 200 with nothing behind it. The decoder must refuse before writing past the caller's four-entry stack array.
    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
    _nya_net_write_varint(payload, 0);
    nya_string_push_back(payload, 200);

    SEND_AS_CLIENT(payload);

    nya_net_server_tick(21, 1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(nya_net_server_peer_count() == 0, "a peer sending a malformed command was not dropped");
    printf("  a command claiming 200 entries dropped the peer\n");

    nya_net_server_stop();
  }

  printf("TEST: commands sent faster than ticks move nobody faster, and get the sender kicked\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = 1, .on_spawn_player = nya_callback(spawn_cheater), .on_apply_command = nya_callback(walk) }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "speedhack" });
    nya_object_add(hello, "tick", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 1000 });

    NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));
    SEND_AS_CLIENT(hello_payload);

    nya_net_server_tick(30, CHEAT_TICK);
    nya_system_sim_apply_commands();

    NYA_NetPeerId peer = nya_net_server_local_peer();
    nya_assert(nya_net_peer_is_set(peer) && nya_entity_is_valid(CHEATER));

    // four fresh ticks a packet and four packets a tick: sixteen ticks of walking claimed for every one that passes.
    u64 claimed = 1000;
    u32 ticks   = 0;
    u32 peak    = 0;

    for (; ticks < 120 && nya_net_server_peer_count() == 1; ticks++) {
      for (u32 packet = 0; packet < 4; packet++) {
        NYA_NetCommand run[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
        for (u32 i = 0; i < NYA_NET_COMMAND_REDUNDANCY; i++) run[i] = (NYA_NetCommand){ .tick = ++claimed, .actions = 1 };

        NYA_String* payload = nya_string_create(arena);
        nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
        _nya_net_write_varint(payload, 0);
        NYA_EXPECT(nya_net_command_encode(payload, run, NYA_NET_COMMAND_REDUNDANCY));

        SEND_AS_CLIENT(payload);
      }

      nya_net_server_tick(31 + ticks, CHEAT_TICK);

      if (nya_net_server_peer_count() == 1) peak = nya_max(peak, nya_net_server_peer_stats(peer).violations);

      NYA_Entity* entity = nya_entity_get(CHEATER);
      if (entity != nullptr) {
        f32 honest_most = (f32)(ticks + 1 + 4) * HONEST_SPEED * CHEAT_TICK;
        nya_assert(entity->position.x <= honest_most + 0.01F, "after %u ticks the cheater is at %f, past the %f an honest client reaches", ticks + 1,
                   (f64)entity->position.x, (f64)honest_most);
      }

      nya_system_sim_apply_commands();
    }

    printf("  sixteen ticks claimed per tick: kicked after %u ticks with %u violations counted\n", ticks, peak);

    nya_assert(nya_net_server_peer_count() == 0, "a client flooding commands was never kicked");

    nya_net_server_stop();
  }

  printf("TEST: max_speed holds a game that trusts the command to the server's rules\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag  = 1,
      .on_spawn_player  = nya_callback(spawn_cheater),
      .on_apply_command = nya_callback(walk_trusting),
      .max_speed        = 200.0F,
    }));

    NYA_NetTransport* hostile = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&hostile));

    NYA_String* hello_payload = nya_string_create(arena);
    nya_net_message_begin(hello_payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(arena);
    nya_object_add(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_add(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_add(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "teleporter" });

    NYA_EXPECT(nya_net_message_write_object(arena, hello_payload, hello));
    SEND_AS_CLIENT(hello_payload);

    nya_net_server_tick(1, CHEAT_TICK);
    nya_system_sim_apply_commands();

    NYA_NetPeerId peer = nya_net_server_local_peer();

    for (u32 i = 0; i < 10; i++) {
      NYA_NetCommand fast = { .tick = 1 + i, .analog = 100000.0F };

      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
      _nya_net_write_varint(payload, 0);
      NYA_EXPECT(nya_net_command_encode(payload, &fast, 1));
      SEND_AS_CLIENT(payload);

      nya_net_server_tick(2 + i, CHEAT_TICK);
      nya_system_sim_apply_commands();
    }

    NYA_Entity* entity = nya_entity_get(CHEATER);

    f32 limit = 10.0F * 200.0F * CHEAT_TICK * 1.01F;

    printf("  ten commands asking for 100000 units a second: moved %.2f, violations %u\n", (f64)entity->position.x, nya_net_server_peer_stats(peer).violations);

    nya_assert(entity->position.x <= limit, "max_speed let the entity reach %f past a limit of %f", (f64)entity->position.x, (f64)limit);
    nya_assert(entity->position.x > 0.0F, "max_speed stopped the entity instead of slowing it");
    nya_assert(nya_net_server_peer_stats(peer).violations == 10, "each clamped command should count once");

    nya_net_server_stop();
  }

  printf("TEST: random application payloads never fault the server\n");
  {
    /* The catch-all for the message layer, mirroring the transport one above. Every message kind reached with garbage behind it, including the kinds only valid in the other direction. */
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

  nya_os_socket_stop();

  printf("PASSED: test_attack (0 failures)\n");

  return EXIT_SUCCESS;
}
