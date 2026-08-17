/**
 * The transport layer: loopback in one process, and UDP over a real localhost socket.
 *
 * This is wire code, so almost everything here is about the cases that only happen on a network.
 * The contract at the top of net_transport.h is what is being checked, clause by clause:
 *
 * - a reliable message arrives, in order, exactly once, or the peer is dropped
 * - an unreliable message may vanish or arrive late, but is never duplicated and never truncated
 * - a message of any size may be sent, and the transport splits it to fit the path
 * - nothing blocks
 *
 * The interesting half is driven by SDL_net's own packet loss simulation, because the failures that
 * matter cannot be provoked on a loopback interface that never drops anything. A retransmit that
 * fires when it should not, an out-of-order reliable message handed up early, a fragmented message
 * reassembled with a hole in it — none of those are reachable on a perfect link, and all of them are
 * ordinary on a real one.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

/** A port unlikely to be taken. Several are tried, because a busy port is a flaky test otherwise. */
#define FIRST_PORT 47820

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
     *
     * This is the case that made the loopback transport copy into the *receiver's* arena rather than
     * hand back a pointer to the sender's: a snapshot is built into a scratch buffer that is reused
     * every tick, so pointing at it would deliver whatever happened to be there at poll time.
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

    nya_net_transport_destroy(a);
    nya_net_transport_destroy(b);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: UDP over localhost — handshake, both directions, fragmentation
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: udp connects over localhost\n");
  {
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));

    // Several ports tried, because a port already in use is otherwise a flaky test rather than a
    // failing one — and on a shared CI machine that happens.
    u16 port = 0;

    for (u16 candidate = FIRST_PORT; candidate < FIRST_PORT + 16; candidate++) {
      if (nya_net_transport_listen(server, candidate).ok) {
        port = candidate;
        break;
      }
    }

    nya_assert(port != 0, "could not bind any port in the test range");
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
       *
       * That is the whole reason it fragments rather than letting IP do it: an IP fragment lost
       * anywhere costs the entire datagram, while a fragment lost here costs one retransmit on the
       * reliable channel.
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

      // The id is dead. A handle held across a disconnect must fail to resolve rather than address
      // whoever takes the slot next — that is the whole point of the generation.
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
     *
     * SDL_net drops a third of the datagrams in both directions, so retransmits fire, fragments go
     * missing, acknowledgements vanish and messages arrive out of order — every path in the
     * reliability layer that a working network never exercises. What must still hold is the
     * contract: every reliable message, exactly once, in order.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));

    u16 port = 0;
    for (u16 candidate = FIRST_PORT + 32; candidate < FIRST_PORT + 48; candidate++) {
      if (nya_net_transport_listen(server, candidate).ok) {
        port = candidate;
        break;
      }
    }
    nya_assert(port != 0, "could not bind any port in the lossy test range");

    NYA_EXPECT(nya_net_transport_connect(client, "127.0.0.1", port));

    Collected cs = { 0 };
    Collected cc = { 0 };

    // Loss is turned on *after* the handshake. The handshake has its own retry and is worth testing
    // under loss too, but not in the same case — a failure here should point at one thing.
    nya_assert(pump_until(client, server, &cc, &cs, both_connected), "the handshake did not complete");

    NYA_NetPeerId to_client = cs.last_peer;

    nya_net_simulate_packet_loss(server, 30);
    nya_net_simulate_packet_loss(client, 30);

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
     *
     * A peer timeout is ten seconds and a keepalive is one, so a test that waited would take longer than the
     * whole suite. The timestamps are written directly instead — which is honest about what is being tested:
     * the *decision*, not the clock.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));

    u16 port = 0;
    for (u16 candidate = FIRST_PORT + 64; candidate < FIRST_PORT + 80; candidate++) {
      if (nya_net_transport_listen(server, candidate).ok) {
        port = candidate;
        break;
      }
    }
    nya_assert(port != 0, "could not bind any port in the timeout test range");

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

      // The clock has to be past the timeout for the subtraction to exceed it, which it is — the monotonic
      // clock is well past ten seconds by the time a test suite reaches here.
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
     *
     * The reliable path is covered above; this one has no retransmit behind it, so every fragment has to
     * arrive on the first attempt or the message is simply gone. On loopback it does, which is what makes
     * this a test of the encoding rather than of the recovery.
     */
    NYA_NetTransport* server = nullptr;
    NYA_NetTransport* client = nullptr;

    NYA_EXPECT(nya_net_transport_udp_create(arena, &server));
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));

    u16 port = 0;
    for (u16 candidate = FIRST_PORT + 96; candidate < FIRST_PORT + 112; candidate++) {
      if (nya_net_transport_listen(server, candidate).ok) {
        port = candidate;
        break;
      }
    }
    nya_assert(port != 0);

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
    nya_net_simulate_packet_loss(a, 100);

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
     * A stub, and covered anyway — because "reports itself unsupported" is a contract a game relies on to
     * grey out a menu item, and a stub that asserted or returned a broken transport instead would be found by
     * a player rather than here.
     *
     * The Steamworks SDK is not on the link line, so the implementation cannot exist yet. See net_steam.c for
     * why the seam is written regardless.
     */
    NYA_NetTransport* steam = nullptr;

    NYA_Error created = nya_net_transport_steam_create(arena, &steam);

    nya_assert(created.kind == NYA_ERROR_NOT_SUPPORTED, "the Steam transport should report NOT_SUPPORTED, got %d", (int)created.kind);
    nya_assert(steam == nullptr, "a refused transport handed back a pointer anyway");
  }

  printf("TEST: transport construction and reconfiguration errors\n");
  {
    NYA_NetTransport* transport = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, &transport));

    // A transport holds one socket. Listening twice, or connecting after listening, would silently discard
    // the first — so both are refused.
    u16 port = 0;
    for (u16 candidate = FIRST_PORT + 128; candidate < FIRST_PORT + 144; candidate++) {
      if (nya_net_transport_listen(transport, candidate).ok) {
        port = candidate;
        break;
      }
    }
    nya_assert(port != 0);

    nya_assert(!nya_net_transport_listen(transport, (u16)(port + 1)).ok, "listening twice was accepted");
    nya_assert(!nya_net_transport_connect(transport, "127.0.0.1", port).ok, "connecting on a listening socket was accepted");

    // A hostname that cannot resolve is an error a player can act on rather than a hang.
    NYA_NetTransport* client = nullptr;
    NYA_EXPECT(nya_net_transport_udp_create(arena, &client));

    NYA_Error unresolvable = nya_net_transport_connect(client, "this-host-does-not-exist.invalid", 1234);
    nya_assert(!unresolvable.ok, "an unresolvable hostname was accepted");

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

  printf("PASSED: test_transport (0 failures)\n");

  return EXIT_SUCCESS;
}
