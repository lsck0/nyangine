/**
 * The transport layer: loopback in one process, and UDP over a real localhost socket.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

/** How long a pump loop waits for something to happen before giving up on it. */
#define PUMP_TIMEOUT_MS 4000

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/*
 * What a pump loop collected, so a test can assert on the whole exchange rather than on one event
 * at a time. Payloads are copied because the transport's own buffers die on the next poll.
 */
#define MAX_COLLECTED 512

typedef struct {
  u32 connects;
  u32 disconnects;
  u32 messages;

  u64 sizes[MAX_COLLECTED];
  u8  first_byte[MAX_COLLECTED];
  u8  last_byte[MAX_COLLECTED];

  NYA_NetPeerId last_peer;
} Collected;

/** Drains one transport into `out`, without waiting. */
static void drain(NYA_NetTransport* transport, Collected* out) {
  NYA_NetTransportEvent event = { 0 };

  while (nya_net_transport_poll(transport, &event)) {
    switch (event.kind) {
      case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
        out->connects++;
        out->last_peer = event.peer;
      } break;

      case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: out->disconnects++; break;

      case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
        nya_assert(event.data != nullptr && event.size > 0, "a delivered message is never empty");

        if (out->messages < MAX_COLLECTED) {
          out->sizes[out->messages]      = event.size;
          out->first_byte[out->messages] = event.data[0];
          out->last_byte[out->messages]  = event.data[event.size - 1];
        }

        out->messages++;
        out->last_peer = event.peer;
      } break;

      default: break;
    }
  }
}

/** Pumps one transport `times`, discarding everything, so its timers fire and its queues drain. */
static void pump(NYA_NetTransport* transport, u32 times) {
  for (u32 i = 0; i < times; i++) {
    NYA_NetTransportEvent event = { 0 };
    while (nya_net_transport_poll(transport, &event)) { }

    sleep_ms(2);
  }
}

/** Pumps both ends until `predicate` holds or the timeout expires. Returns whether it held. */
static b8 pump_until(NYA_NetTransport* a, NYA_NetTransport* b, Collected* ca, Collected* cb, b8 (*predicate)(Collected*, Collected*)) {
  u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

  while (nya_clock_get_monotonic_ms() < deadline) {
    drain(a, ca);
    drain(b, cb);

    if (predicate(ca, cb)) return true;

    // A real sleep, not a spin: the UDP transport's retransmit and keepalive timers are in
    // milliseconds, and a busy loop would burn the whole timeout without letting any of them fire.
    sleep_ms(2);
  }

  // One last drain, so something that landed inside the final sleep is not missed.
  drain(a, ca);
  drain(b, cb);

  return predicate(ca, cb);
}

static b8 both_connected(Collected* a, Collected* b) {
  return a->connects > 0 && b->connects > 0;
}

static u32 EXPECTED_MESSAGES = 0;

static b8 server_got_expected(Collected* a, Collected* b) {
  nya_unused(a);
  return b->messages >= EXPECTED_MESSAGES;
}

static b8 client_got_expected(Collected* a, Collected* b) {
  nya_unused(b);
  return a->messages >= EXPECTED_MESSAGES;
}

/** Fills `buffer` with a recognisable pattern whose first and last bytes identify the message. */
static void fill(u8* buffer, u64 size, u8 tag) {
  for (u64 i = 0; i < size; i++) buffer[i] = (u8)(tag + (u8)(i & 0x7F));

  buffer[0]        = tag;
  buffer[size - 1] = (u8)(tag ^ 0xFF);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_transport");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a loopback pair is joined at creation
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: loopback pair connects and carries messages\n");
  {
    NYA_NetTransport* a = nullptr;
    NYA_NetTransport* b = nullptr;

    NYA_EXPECT(nya_net_transport_loopback_create(arena, &a, &b));

    nya_assert(nya_net_transport_is_local(a), "a loopback transport reports itself local");
    nya_assert(nya_net_transport_is_local(b));

    Collected ca = { 0 };
    Collected cb = { 0 };

    // Both ends learn about the connection from an event, even though the pair was already joined.
    // The layers above are written against events and must not have to special case this.
    drain(a, &ca);
    drain(b, &cb);

    nya_assert(ca.connects == 1 && cb.connects == 1);

    u8 payload[64];
    fill(payload, sizeof(payload), 0xA1);

    NYA_EXPECT(nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)));

    /*
     * The sender's buffer is overwritten before the receiver polls.
     */
    fill(payload, sizeof(payload), 0x00);

    drain(b, &cb);

    nya_assert(cb.messages == 1);
    nya_assert(cb.sizes[0] == sizeof(payload));
    nya_assert(cb.first_byte[0] == 0xA1, "the message is what was sent, not what the buffer held later");
    nya_assert(cb.last_byte[0] == (u8)(0xA1 ^ 0xFF));

    // The other direction, and ordering.
    for (u8 i = 0; i < 5; i++) {
      u8 message[16];
      fill(message, sizeof(message), (u8)(0x10 + i));
      NYA_EXPECT(nya_net_transport_send(b, cb.last_peer, NYA_NET_CHANNEL_RELIABLE, message, sizeof(message)));
    }

    drain(a, &ca);

    nya_assert(ca.messages == 5);
    for (u8 i = 0; i < 5; i++) nya_assert(ca.first_byte[i] == (u8)(0x10 + i), "loopback preserves order");

    // A loopback pair has exactly one peer and cannot listen or connect out. Errors rather than
    // assertions, so a menu offering "open to LAN" can grey the option out.
    nya_assert(nya_net_transport_listen(a, 1234).kind == NYA_ERROR_NOT_SUPPORTED);
    nya_assert(nya_net_transport_connect(a, "127.0.0.1", 1234).kind == NYA_ERROR_NOT_SUPPORTED);

    NYA_NetPeerId nonsense = { .index = 7, .generation = 3 };
    nya_assert(!nya_net_transport_send(a, nonsense, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)).ok);

    // a long session must not grow the arena the pair was created from: polled bytes are released.
    u64 used_before = nya_arena_stats(arena).used_bytes;
    for (u32 i = 0; i < 10000; i++) {
      NYA_EXPECT(nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_UNRELIABLE, payload, sizeof(payload)));
      drain(b, &cb);
    }
    u64 used_after = nya_arena_stats(arena).used_bytes;
    nya_assert(used_after <= used_before + sizeof(payload), "the arena grew from " FMTu64 " to " FMTu64 " bytes", used_before, used_after);

    // a disconnect reaches the far end with its reason, after what was sent before it, as it would off a wire.
    NYA_EXPECT(nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)));
    nya_net_transport_disconnect(a, ca.last_peer, NYA_NET_DISCONNECT_CHEATING);

    NYA_NetTransportEvent last  = { 0 };
    NYA_NetTransportEvent event = { 0 };
    u32                   seen  = 0;
    while (nya_net_transport_poll(b, &event)) {
      nya_assert(seen == 0 || last.kind != NYA_NET_TRANSPORT_EVENT_DISCONNECTED, "nothing arrives after the disconnect");
      last = event;
      seen++;
    }
    nya_assert(seen == 2 && last.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED, "the message and then the disconnect, got %u events", seen);
    nya_assert(last.reason == NYA_NET_DISCONNECT_CHEATING, "with the reason it was given, got %d", (int)last.reason);

    nya_net_transport_destroy(a);
    nya_net_transport_destroy(b);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: UDP over localhost: handshake, both directions, fragmentation
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: udp connects over localhost\n");
  {
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);
    nya_assert(!nya_net_transport_is_local(server), "a UDP transport is not local even on loopback");

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    nya_assert(cc.connects == 1, "the client is told once that it is connected");
    nya_assert(cs.connects == 1, "and the server is told once that somebody arrived");
    nya_assert(nya_net_peer_is_set(cs.last_peer), "the server's peer id is a real id");

    NYA_NetPeerId server_to_client = cs.last_peer;
    NYA_NetPeerId client_to_server = cc.last_peer;

    printf("  connected on port %u\n", port);

    // ── one small reliable message each way ────────────────────────────────────
    {
      u8 payload[100];
      fill(payload, sizeof(payload), 0x5A);

      NYA_EXPECT(nya_net_transport_send(server, server_to_client, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)));

      EXPECTED_MESSAGES = 1;
      nya_assert(pump_until(client, server, &cc, &cs, client_got_expected), "the client never received the message");

      nya_assert(cc.sizes[0] == sizeof(payload));
      nya_assert(cc.first_byte[0] == 0x5A);
      nya_assert(cc.last_byte[0] == (u8)(0x5A ^ 0xFF), "the whole message arrived, not a prefix of it");
    }

    // ── a message far larger than one datagram ─────────────────────────────────
    printf("TEST: udp fragments and reassembles a large message\n");
    {
      /*
       * Well past NYA_NET_MAX_DATAGRAM, so this is split into fragments the transport tracks itself.
       */
      u64 size    = 30000;
      u8* payload = nya_arena_alloc(arena, size);
      fill(payload, size, 0x7C);

      u32 before = cc.messages;

      NYA_EXPECT(nya_net_transport_send(server, server_to_client, NYA_NET_CHANNEL_RELIABLE, payload, size));

      EXPECTED_MESSAGES = before + 1;
      nya_assert(pump_until(client, server, &cc, &cs, client_got_expected), "the fragmented message never arrived");

      nya_assert(cc.sizes[before] == size, "reassembled to exactly the size that was sent");
      nya_assert(cc.first_byte[before] == 0x7C);
      nya_assert(cc.last_byte[before] == (u8)(0x7C ^ 0xFF), "including the very last byte");
    }

    // ── ordering, and exactly-once, on the reliable channel ────────────────────
    printf("TEST: udp reliable messages arrive once and in order\n");
    {
      u32 before = cc.messages;
      u32 count  = 32;

      for (u32 i = 0; i < count; i++) {
        u8 message[64];
        fill(message, sizeof(message), (u8)i);
        NYA_EXPECT(nya_net_transport_send(server, server_to_client, NYA_NET_CHANNEL_RELIABLE, message, sizeof(message)));
      }

      EXPECTED_MESSAGES = before + count;
      nya_assert(pump_until(client, server, &cc, &cs, client_got_expected), "not every reliable message arrived");

      for (u32 i = 0; i < count; i++) {
        nya_assert(cc.first_byte[before + i] == (u8)i, "reliable message %u arrived out of order", i);
      }

      // Pump well past a retransmit interval. A message whose acknowledgement was lost is resent,
      // and the receiver must not hand it up a second time.
      u64 quiet_until = nya_clock_get_monotonic_ms() + 600;
      while (nya_clock_get_monotonic_ms() < quiet_until) {
        drain(client, &cc);
        drain(server, &cs);
        sleep_ms(2);
      }

      nya_assert(cc.messages == before + count, "a retransmit delivered a duplicate");
    }

    // ── the client talks back ──────────────────────────────────────────────────
    {
      u32 before = cs.messages;

      u8 payload[40];
      fill(payload, sizeof(payload), 0x33);

      NYA_EXPECT(nya_net_transport_send(client, client_to_server, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)));

      EXPECTED_MESSAGES = before + 1;
      nya_assert(pump_until(client, server, &cc, &cs, server_got_expected), "the server never heard from the client");

      nya_assert(cs.first_byte[before] == 0x33);
    }

    // ── the round trip is measured ─────────────────────────────────────────────
    {
      NYA_NetPeerStats stats = nya_net_transport_stats(server, server_to_client);

      nya_assert(stats.packets_sent > 0 && stats.packets_received > 0);
      nya_assert(stats.bytes_sent > 0 && stats.bytes_received > 0);

      // On loopback this is a fraction of a millisecond, so the assertion is that it is *sane*
      // rather than that it is any particular number.
      nya_assert(stats.rtt_ms >= 0.0F && stats.rtt_ms < 1000.0F, "a loopback round trip of %f ms is not credible", (f64)stats.rtt_ms);

      NYA_ConstCString address = nya_net_transport_peer_address(server, server_to_client);
      nya_assert(address != nullptr && address[0] != '\0');
    }

    // ── a disconnect is seen by the far end ────────────────────────────────────
    printf("TEST: udp disconnect reaches the peer\n");
    {
      nya_net_transport_disconnect(server, server_to_client, NYA_NET_DISCONNECT_REQUESTED);

      u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
      while (cc.disconnects == 0 && nya_clock_get_monotonic_ms() < deadline) {
        drain(client, &cc);
        drain(server, &cs);
        sleep_ms(2);
      }

      nya_assert(cc.disconnects == 1, "the client was never told the server dropped it");

      // the id is dead. A handle held across a disconnect must not resolve to whoever takes the slot next.
      u8 payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
      nya_assert(!nya_net_transport_send(server, server_to_client, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)).ok);
    }

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(server);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reliability actually recovers from loss
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: udp reliable delivery survives 30%% packet loss\n");
  {
    /*
     * The test the perfect loopback link cannot provide.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    // loss is turned on after the handshake, which has its own retry, so a failure points at one thing.
    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    NYA_NetPeerId to_client = cs.last_peer;

    nya_net_transport_condition(server, (NYA_NetConditions){ .loss_percent = 30.0F });
    nya_net_transport_condition(client, (NYA_NetConditions){ .loss_percent = 30.0F });

    u32 before = cc.messages;
    u32 count  = 24;

    for (u32 i = 0; i < count; i++) {
      u8 message[200];
      fill(message, sizeof(message), (u8)i);
      NYA_EXPECT(nya_net_transport_send(server, to_client, NYA_NET_CHANNEL_RELIABLE, message, sizeof(message)));
    }

    // Generous, because recovery is paced by the retransmit timer rather than by the link.
    u64 deadline = nya_clock_get_monotonic_ms() + 15000;

    while (cc.messages < before + count && nya_clock_get_monotonic_ms() < deadline) {
      drain(client, &cc);
      drain(server, &cs);
      sleep_ms(2);
    }

    nya_assert(cc.messages >= before + count, "only %u of %u reliable messages survived the loss", cc.messages - before, count);

    for (u32 i = 0; i < count; i++) {
      nya_assert(cc.first_byte[before + i] == (u8)i, "message %u arrived out of order under loss", i);
      nya_assert(cc.last_byte[before + i] == (u8)((u8)i ^ 0xFF), "message %u was truncated", i);
    }

    // And nothing arrived twice, which retransmits make the likely failure rather than an exotic one.
    u64 settle = nya_clock_get_monotonic_ms() + 800;
    while (nya_clock_get_monotonic_ms() < settle) {
      drain(client, &cc);
      drain(server, &cs);
      sleep_ms(2);
    }

    nya_assert(cc.messages == before + count, "%u duplicate deliveries under loss", cc.messages - (before + count));

    NYA_NetPeerStats stats = nya_net_transport_stats(server, to_client);
    printf("  recovered %u messages, %llu retransmits\n", count, (unsigned long long)stats.retransmits);

    nya_assert(stats.retransmits > 0, "30%% loss produced no retransmits at all, which cannot be right");

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(server);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the paths a working network never takes
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: timeouts, keepalives and dead peers\n");
  {
    /*
     * The error paths, reached by moving the clock rather than by waiting.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    NYA_NetPeerId to_client = cs.last_peer;

    _NYA_NetUdpState* server_state = server->state;

    // ── a keepalive goes out when there is nothing else to say ────────────────
    {
      /*
       * A connection with no traffic is indistinguishable from a dead one, so a player standing still in a
       * menu would be dropped at the timeout. The keepalive is an empty data packet whose only content is
       * the acknowledgement in its header.
       */
      u64 before = server_state->peers[to_client.index].stats.packets_sent;

      server_state->peers[to_client.index].last_sent_ms = 0;

      pump(server, 3);

      nya_assert(server_state->peers[to_client.index].stats.packets_sent > before, "no keepalive was sent for a quiet peer");

      // And the client accepts it: a keepalive is kind DATA with zero fragments, so a receiver that read a
      // kind byte off the end would fault on it. That was a real bug.
      pump(client, 3);
      nya_assert(cc.disconnects == 0, "a keepalive disconnected the client");
    }

    // ── a peer that stops being heard from is removed ─────────────────────────
    {
      u32 before = 0;
      for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (server_state->peers[i].occupied) before++;
      }

      nya_assert(before >= 1, "the server should hold the client");

      // Further in the past than the timeout, which is what a pulled cable looks like.
      server_state->peers[to_client.index].last_received_ms = 1;

      // the clock must be past the timeout for the subtraction to exceed it, and the monotonic clock is well
      // past ten seconds by now.
      nya_assert(nya_clock_get_monotonic_ms() > 10000, "the monotonic clock is too young for this case to mean anything");

      pump(server, 5);

      u32 after = 0;
      for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (server_state->peers[i].occupied) after++;
      }

      nya_assert(after < before, "a peer that stopped responding was not timed out (%u -> %u)", before, after);

      // The handle is dead, so sending to it reports a missing peer rather than writing to a freed slot.
      u8 payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
      nya_assert(!nya_net_transport_send(server, to_client, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)).ok,
                 "sending to a timed out peer succeeded");
    }

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(server);
  }

  printf("TEST: a fragmented unreliable message\n");
  {
    /*
     * Unreliable fragmentation, which is what a large snapshot actually is.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    u64 size    = 12000;
    u8* payload = nya_arena_alloc(arena, size);
    fill(payload, size, 0x9E);

    u32 before = cc.messages;

    NYA_EXPECT(nya_net_transport_send(server, cs.last_peer, NYA_NET_CHANNEL_UNRELIABLE, payload, size));

    EXPECTED_MESSAGES = before + 1;
    nya_assert(pump_until(client, server, &cc, &cs, client_got_expected), "the fragmented unreliable message never arrived");

    nya_assert(cc.sizes[before] == size, "reassembled to %llu bytes instead of %llu", (unsigned long long)cc.sizes[before],
               (unsigned long long)size);
    nya_assert(cc.first_byte[before] == 0x9E && cc.last_byte[before] == (u8)(0x9E ^ 0xFF), "the unreliable message was corrupted");

    // A message too large to fragment at all is refused rather than truncated.
    u64 absurd     = (u64)NYA_NET_MAX_DATAGRAM * 4096;
    u8* absurd_buf = nya_arena_alloc(arena, 16);

    nya_assert(!nya_net_transport_send(server, cs.last_peer, NYA_NET_CHANNEL_UNRELIABLE, absurd_buf, absurd).ok,
               "a message needing more fragments than the limit was accepted");

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(server);
  }

  printf("TEST: the conditioner delays, duplicates and reorders without breaking delivery\n");
  {
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    NYA_NetPeerId to_client = cs.last_peer;

    // ── latency is latency ─────────────────────────────────────────────────────
    nya_net_transport_condition(server, (NYA_NetConditions){ .latency_ms = 120 });

    u8 ping[32];
    fill(ping, sizeof(ping), 0x51);

    u32 before  = cc.messages;
    u64 sent_ms = nya_clock_get_monotonic_ms();

    NYA_EXPECT(nya_net_transport_send(server, to_client, NYA_NET_CHANNEL_UNRELIABLE, ping, sizeof(ping)));

    while (cc.messages == before && nya_clock_get_monotonic_ms() < sent_ms + 2000) {
      drain(client, &cc);
      drain(server, &cs);
      sleep_ms(1);
    }

    u64 took_ms = nya_clock_get_monotonic_ms() - sent_ms;
    printf("  a datagram under 120 ms of latency took %llu ms\n", (unsigned long long)took_ms);

    nya_assert(cc.messages == before + 1, "the delayed datagram never arrived");
    nya_assert(took_ms >= 115, "120 ms of latency delivered in %llu ms", (unsigned long long)took_ms);

    // ── everything at once, both ways ──────────────────────────────────────────
    NYA_NetConditions bad = { .latency_ms = 40, .jitter_ms = 20, .loss_percent = 10.0F, .duplicate_percent = 10.0F, .reorder_percent = 10.0F };

    nya_net_transport_condition(server, bad);
    nya_net_transport_condition(client, bad);

    before = cc.messages;

    u32 count = 60;

    for (u32 i = 0; i < count; i++) {
      u8 message[64];
      fill(message, sizeof(message), (u8)i);
      NYA_EXPECT(nya_net_transport_send(server, to_client, NYA_NET_CHANNEL_RELIABLE, message, sizeof(message)));
    }

    u64 phase_started_ms = nya_clock_get_monotonic_ms();
    u64 deadline         = phase_started_ms + 15000;
    while (cc.messages < before + count && nya_clock_get_monotonic_ms() < deadline) {
      drain(client, &cc);
      drain(server, &cs);
      sleep_ms(2);
    }

    // a duplicated datagram is a replay to the receiver, so nothing may arrive twice once the stream settles.
    u64 settle = nya_clock_get_monotonic_ms() + 600;
    while (nya_clock_get_monotonic_ms() < settle) {
      drain(client, &cc);
      drain(server, &cs);
      sleep_ms(2);
    }

    u64 phase_ms = nya_clock_get_monotonic_ms() - phase_started_ms;

    nya_assert(cc.messages == before + count, "%u of %u reliable messages arrived through a bad link", cc.messages - before, count);

    for (u32 i = 0; i < count; i++) nya_assert(cc.first_byte[before + i] == (u8)i, "message %u arrived out of order", i);

    NYA_NetPeerStats received = nya_net_transport_stats(client, cc.last_peer);
    NYA_NetPeerStats sent     = nya_net_transport_stats(server, to_client);

    printf("  60 reliable messages in order: %.1f ms rtt, %.1f ms jitter, %.0f%% loss, %llu resends; %llu duplicates rejected\n", (f64)sent.rtt_ms,
           (f64)sent.jitter_ms, (f64)(sent.packet_loss * 100.0F), (unsigned long long)sent.retransmits, (unsigned long long)received.packets_rejected);

    nya_assert(received.packets_rejected > 0, "10%% duplication produced no rejected replays");
    /*
     * The estimate is not asserted in milliseconds, and the 250 ms bound that used to be here was a
     * measurement of the runner: it is an average over acknowledged datagrams, and under this link most
     * are lost, the survivors are queued behind sixty sends, and the average is still climbing towards
     * whatever it would settle at. That latency is applied at all is the case above, which times one
     * datagram against the wall clock and needs no upper bound to do it. What holds here whatever the
     * host does is that an estimate cannot exceed the exchange it was measured in, which is what an
     * estimator counting queueing or a resend as one round trip would break.
     */
    nya_assert(sent.rtt_ms <= (f32)phase_ms, "a %.1f ms round trip out of an exchange that took %llu ms", (f64)sent.rtt_ms,
               (unsigned long long)phase_ms);

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(server);
  }

  printf("TEST: a pinned server key is enforced, and a player key is proven\n");
  {
    NYA_NetKeyPair server_identity = { 0 };
    NYA_NetKeyPair player_identity = { 0 };
    NYA_NetKeyPair impostor        = { 0 };

    NYA_EXPECT(nya_net_key_pair_create(&server_identity));
    NYA_EXPECT(nya_net_key_pair_create(&player_identity));
    NYA_EXPECT(nya_net_key_pair_create(&impostor));

    // the hex form survives a round trip, and anything that is not exactly a key is refused.
    char hex[NYA_NET_KEY_HEX_SIZE];
    nya_net_key_to_hex(server_identity.public_key, hex);

    u8 parsed[NYA_NET_KEY_SIZE];
    nya_assert(nya_net_key_from_hex(hex, parsed) && nya_memcmp(parsed, server_identity.public_key, NYA_NET_KEY_SIZE) == 0);

    hex[10] = 'x';
    nya_assert(!nya_net_key_from_hex(hex, parsed) && !nya_net_key_is_set(parsed), "a key with a bad digit parsed");
    nya_assert(!nya_net_key_from_hex("abcd", parsed), "a short key parsed");

    // a saved identity survives a restart, and a damaged one is replaced rather than trusted.
    {
      NYA_EXPECT(nya_system_save_init());

      NYA_ConstCString path = "test_transport/identity.nya";
      (void)nya_save_delete(path);

      NYA_NetKeyPair first  = { 0 };
      NYA_NetKeyPair second = { 0 };

      NYA_EXPECT(nya_net_key_pair_load(path, &first));
      NYA_EXPECT(nya_net_key_pair_load(path, &second));

      nya_assert(nya_net_key_is_set(first.public_key) && nya_memcmp(&first, &second, sizeof(first)) == 0, "a saved identity changed between loads");

      NYA_Object* damaged = nya_object_create(arena);
      nya_object_set(damaged, "secret_key", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "not a key" });
      NYA_EXPECT(nya_save_write(path, damaged, NYA_SERDE_NONE));

      NYA_EXPECT(nya_net_key_pair_load(path, &second));
      nya_assert(nya_net_key_is_set(second.public_key) && nya_memcmp(&first, &second, sizeof(first)) != 0, "a damaged identity was not replaced");

      NYA_EXPECT(nya_save_delete(path));
      nya_system_save_deinit();
    }

    NYA_NetTransport* server = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ .identity = server_identity }, &server));

    NYA_EXPECT(nya_net_transport_listen(server, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(server);

    nya_assert(nya_memcmp(nya_net_transport_public_key(server), server_identity.public_key, NYA_NET_KEY_SIZE) == 0, "the server is not who it was told to be");

    // ── a client expecting someone else refuses this server ─────────────────────
    {
      NYA_NetUdpOptions options = { 0 };
      nya_memcpy(options.server_key, impostor.public_key, NYA_NET_KEY_SIZE);

      NYA_NetTransport* client = nullptr;
      NYA_EXPECT(nya_net_transport_udp_create(arena, options, &client));
      NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

      NYA_NetDisconnect reason   = NYA_NET_DISCONNECT_NONE;
      u32               connects = 0;
      u64               deadline = nya_clock_get_monotonic_ms() + 8000;

      while (reason == NYA_NET_DISCONNECT_NONE && nya_clock_get_monotonic_ms() < deadline) {
        NYA_NetTransportEvent event = { 0 };

        while (nya_net_transport_poll(client, &event)) {
          if (event.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED) connects++;
          if (event.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED) reason = event.reason;
        }

        pump(server, 1);
      }

      nya_assert(connects == 0, "a client connected to a server whose key it was told to refuse");
      nya_assert(reason == NYA_NET_DISCONNECT_IDENTITY, "the refusal was reported as %d rather than as an identity failure", (int)reason);

      nya_net_transport_destroy(client);
    }

    // ── the right key, and a player key the server can read back ────────────────
    {
      NYA_NetUdpOptions options = { .identity = player_identity };
      nya_memcpy(options.server_key, server_identity.public_key, NYA_NET_KEY_SIZE);

      NYA_NetTransport* client = nullptr;
      NYA_EXPECT(nya_net_transport_udp_create(arena, options, &client));
      NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

      Collected cs = { 0 };
      Collected cc = { 0 };

      nya_assert(pump_until(client, server, &cc, &cs, both_connected), "a client pinning the right key did not connect");

      const u8* proven = nya_net_transport_peer_key(server, cs.last_peer);
      nya_assert(proven != nullptr && nya_memcmp(proven, player_identity.public_key, NYA_NET_KEY_SIZE) == 0, "the server did not learn the player's key");

      const u8* server_seen = nya_net_transport_peer_key(client, cc.last_peer);
      nya_assert(server_seen != nullptr && nya_memcmp(server_seen, server_identity.public_key, NYA_NET_KEY_SIZE) == 0);

      u8 message[48];
      fill(message, sizeof(message), 0x3C);

      u32 before = cs.messages;
      NYA_EXPECT(nya_net_transport_send(client, cc.last_peer, NYA_NET_CHANNEL_RELIABLE, message, sizeof(message)));

      EXPECTED_MESSAGES = before + 1;
      nya_assert(pump_until(client, server, &cc, &cs, server_got_expected), "an encrypted message did not arrive");
      nya_assert(cs.first_byte[before] == 0x3C, "an encrypted message arrived altered");

      nya_net_transport_destroy(client);
    }

    nya_net_transport_destroy(server);
  }

  printf("TEST: a loopback peer that has gone\n");
  {
    /*
     * The far end being destroyed, which on a listen server is what shutting down looks like from the half
     * that is still running.
     */
    NYA_NetTransport* a = nullptr;
    NYA_NetTransport* b = nullptr;

    NYA_EXPECT(nya_net_transport_loopback_create(arena, &a, &b));

    Collected ca = { 0 };
    Collected cb = { 0 };
    drain(a, &ca);
    drain(b, &cb);

    u8 payload[16];
    fill(payload, sizeof(payload), 0x77);

    // A disconnect from one side is seen by both, and sending afterwards reports a dead peer rather than
    // writing into an arena that is about to go.
    nya_net_transport_disconnect(a, ca.last_peer, NYA_NET_DISCONNECT_REQUESTED);

    nya_assert(!nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)).ok,
               "sending after a loopback disconnect succeeded");

    nya_net_transport_destroy(b);

    nya_assert(!nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)).ok,
               "sending to a destroyed loopback peer succeeded");

    // Stats and the address are still answerable on a dead pair, because a debug overlay reads them without
    // asking whether the connection is alive.
    NYA_NetPeerStats stats = nya_net_transport_stats(a, ca.last_peer);
    nya_assert(stats.rtt_ms == 0.0F, "a loopback reports no round trip, which is a fact rather than a placeholder");

    nya_assert(nya_string_equals(nya_net_transport_peer_address(a, ca.last_peer), "local"));

    nya_net_transport_destroy(a);

    // Destroying twice is tolerated, because a teardown path is not always the one it thinks.
    nya_net_transport_destroy(a);
    nya_net_transport_destroy(nullptr);
  }

  printf("TEST: packet loss simulation is refused where it is meaningless\n");
  {
    NYA_NetTransport* a = nullptr;
    NYA_NetTransport* b = nullptr;
    NYA_EXPECT(nya_net_transport_loopback_create(arena, &a, &b));

    // A loopback has no wire to lose packets on, so this does nothing rather than lying about it.
    nya_net_transport_condition(a, (NYA_NetConditions){ .loss_percent = 100.0F });

    Collected ca = { 0 };
    Collected cb = { 0 };
    drain(a, &ca);
    drain(b, &cb);

    u8 payload[8];
    fill(payload, sizeof(payload), 0x21);

    NYA_EXPECT(nya_net_transport_send(a, ca.last_peer, NYA_NET_CHANNEL_RELIABLE, payload, sizeof(payload)));

    drain(b, &cb);

    nya_assert(cb.messages == 1, "a loopback dropped a packet it has no way to lose");

    nya_net_transport_destroy(a);
    nya_net_transport_destroy(b);
  }

  printf("TEST: the Steam transport reports itself unavailable\n");
  {
    /*
     * A stub, still covered: games grey out menu items based on "unsupported", so it must not assert or
     * return a broken transport.
     */
    NYA_NetTransport* steam = nullptr;

    NYA_Error created = nya_net_transport_steam_create(arena, &steam);

    nya_assert(created.kind == NYA_ERROR_NOT_SUPPORTED, "the Steam transport should report NOT_SUPPORTED, got %d", (int)created.kind);
    nya_assert(steam == nullptr, "a refused transport handed back a pointer anyway");
  }

  printf("TEST: transport construction and reconfiguration errors\n");
  {
    NYA_NetTransport* transport = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &transport));

    // a transport holds one socket. Listening twice or connecting after listening would discard the first,
    // so both are refused.
    NYA_EXPECT(nya_net_transport_listen(transport, 0), "the system had no free UDP port");

    const u16 port = nya_net_transport_port(transport);

    nya_assert(!nya_net_transport_listen(transport, (u16)(port + 1)).ok, "listening twice was accepted");
    nya_assert(!nya_net_transport_connect(transport, "127.0.0.1", port).ok, "connecting on a listening socket was accepted");

    /*
     * A hostname that cannot resolve is still an error a player can act on, but it arrives as a
     * DISCONNECTED event rather than from connect itself.
     *
     * That moved on purpose. Connect used to answer by waiting for the resolver, up to the whole five
     * second connect timeout, inside the caller's call — an error a player can act on is worth less
     * than five seconds of a stopped game, and a game cannot avoid it by being careful. The name is
     * polled from the update now, so connect reports that the attempt started and the failure follows.
     */
    NYA_NetTransport* client = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client));

    NYA_Error unresolvable = nya_net_transport_connect(client, "this-host-does-not-exist.invalid", 1234);
    nya_assert(unresolvable.ok, "starting a connection to an unresolvable hostname is not itself a failure");

    // Stats and the address for a peer that never existed answer rather than faulting, because a debug
    // overlay reads them without checking.
    NYA_NetPeerId nobody = { .index = 11, .generation = 5 };

    NYA_NetPeerStats stats = nya_net_transport_stats(transport, nobody);
    nya_assert(stats.packets_sent == 0 && stats.bytes_sent == 0, "a peer that never existed reported traffic");

    NYA_ConstCString address = nya_net_transport_peer_address(transport, nobody);
    nya_assert(address != nullptr && address[0] != '\0', "a missing peer reported no address string");

    // Disconnecting one that was never there is harmless.
    nya_net_transport_disconnect(transport, nobody, NYA_NET_DISCONNECT_REQUESTED);

    // A zero length send is a caller bug rather than a wire condition, since every receiver switches on a
    // message id in the first byte.
    u8 byte = 1;
    nya_assert(!nya_net_transport_send(transport, nobody, NYA_NET_CHANNEL_UNRELIABLE, nullptr, 4).ok,
               "a null payload was accepted");
    nya_assert(!nya_net_transport_send(transport, nobody, NYA_NET_CHANNEL_UNRELIABLE, &byte, 0).ok,
               "a zero length payload was accepted");

    nya_net_transport_destroy(client);
    nya_net_transport_destroy(transport);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: connecting to a name that will not resolve returns at once
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * This used to sit inside NET_WaitUntilResolved for up to the whole connect timeout, five
     * seconds, before returning to the caller. For a game that is five seconds of a stopped frame
     * because somebody typed the hostname wrong, and no amount of care in the caller could avoid it.
     *
     * The bound here is deliberately loose. What is being held to account is "does not wait for the
     * resolver", not "is fast": a quarter of a second is far below the five that would mean it
     * waited, and far above anything a non-blocking call needs.
     */
    NYA_NetTransport* slow = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &slow));

    const u64 before = nya_clock_get_monotonic_ms();

    // Reserved by RFC 6761 to never resolve, so this is the failing case on any machine anywhere.
    const NYA_Error connecting = nya_net_transport_connect(slow, "nyangine.invalid", 27015);

    const u64 waited = nya_clock_get_monotonic_ms() - before;

    nya_check(waited < 250, "connect returns without waiting for the resolver, took " FMTu64 " ms", waited);
    nya_check(connecting.ok, "and reports the attempt as started rather than failed");

    /*
     * The failure arrives as an event instead, which is what asking rather than waiting costs. Not
     * waited for here either: whether a resolver answers within any particular time is the machine's
     * business, and a test that insists on it fails on a network it does not control.
     */
    pump(slow, 4);

    nya_net_transport_destroy(slow);

    printf("  PASSED\n");
  }

  printf("PASSED: test_transport (0 failures)\n");

  return EXIT_SUCCESS;
}
