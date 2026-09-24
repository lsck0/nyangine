/**
 * The sequence and acknowledgement arithmetic, at unit level.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** How many packets back the bitfield reaches, restated so the test breaks if the field changes width. */
#define ACK_WINDOW 32

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  nya_backtrace_init();
  defer nya_backtrace_deinit();

  // TEST: which of two wrapping sequence numbers is newer
  printf("TEST: sequence comparison across the wrap\n");
  {
    // The ordinary direction.
    nya_assert(_nya_net_udp_sequence_newer(1, 0));
    nya_assert(!_nya_net_udp_sequence_newer(0, 1));
    nya_assert(_nya_net_udp_sequence_newer(1000, 999));

    // Equal is not newer, which matters because _nya_net_udp_record_ack relies on it: a shift of zero would
    // set the wrong bit.
    nya_assert(!_nya_net_udp_sequence_newer(500, 500), "a sequence is not newer than itself");
    nya_assert(!_nya_net_udp_sequence_newer(0, 0));
    nya_assert(!_nya_net_udp_sequence_newer(65535, 65535));

    /*
     * The wrap, and why this is not a `>`: after 65535 comes 0, and a plain comparison would call every
     * later packet older.
     */
    nya_assert(_nya_net_udp_sequence_newer(0, 65535), "0 must be newer than 65535");
    nya_assert(!_nya_net_udp_sequence_newer(65535, 0));

    nya_assert(_nya_net_udp_sequence_newer(5, 65530), "a few past the wrap is newer than a few before it");
    nya_assert(!_nya_net_udp_sequence_newer(65530, 5));

    /*
     * The threshold, at exactly half the space.
     */
    nya_assert(_nya_net_udp_sequence_newer(32768, 0), "half the space ahead is still newer");
    nya_assert(!_nya_net_udp_sequence_newer(32769, 0), "just past half is read as older");

    // Antisymmetry: for any two distinct sequences exactly one is newer. A scheme that answered "both" or
    // "neither" would make the ack window either double-count or stall.
    const u16 samples[] = { 0, 1, 2, 31, 32, 33, 1000, 32767, 32768, 32769, 65533, 65534, 65535 };

    for (u32 i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
      for (u32 j = 0; j < sizeof(samples) / sizeof(samples[0]); j++) {
        u16 a = samples[i];
        u16 b = samples[j];

        if (a == b) {
          nya_assert(!_nya_net_udp_sequence_newer(a, b) && !_nya_net_udp_sequence_newer(b, a), "%u compared to itself", a);
          continue;
        }

        // the one pair exactly half apart in both directions is ambiguous under any scheme, so it is excluded.
        if ((u16)(a - b) == 32768) continue;

        b8 forward  = _nya_net_udp_sequence_newer(a, b);
        b8 backward = _nya_net_udp_sequence_newer(b, a);

        nya_assert(forward != backward, "%u and %u: newer said %d both ways", a, b, (int)forward);
      }
    }
  }

  // TEST: the acknowledgement bitfield
  printf("TEST: the ack bitfield records what arrived\n");
  {
    /*
     * `remote_sequence` is the newest packet seen and `ack_bits` names which of the 32 before it also
     * arrived. Thirty-three packets in six bytes, which is why an acknowledgement rides free on every packet
     * rather than being a message of its own.
     */
    _NYA_NetUdpPeer peer = { 0 };

    // In order, one at a time. Each new packet shifts the field and sets the bit for its predecessor.
    _nya_net_udp_record_ack(&peer, 1);
    nya_assert(peer.remote_sequence == 1);

    _nya_net_udp_record_ack(&peer, 2);
    nya_assert(peer.remote_sequence == 2);
    nya_assert((peer.ack_bits & 1U) != 0, "packet 1 should be one back from 2");

    _nya_net_udp_record_ack(&peer, 3);
    nya_assert(peer.remote_sequence == 3);
    nya_assert((peer.ack_bits & 1U) != 0, "packet 2 is one back");
    nya_assert((peer.ack_bits & 2U) != 0, "packet 1 is two back");
  }

  printf("TEST: an out of order packet sets its own bit\n");
  {
    _NYA_NetUdpPeer peer = { 0 };

    // 10 arrives, then 7: late but inside the window, so still recorded.
    _nya_net_udp_record_ack(&peer, 10);
    _nya_net_udp_record_ack(&peer, 7);

    nya_assert(peer.remote_sequence == 10, "a late packet must not move the newest sequence backwards");
    nya_assert((peer.ack_bits & (1U << 2)) != 0, "packet 7 is three back from 10, so bit 2");

    // The same packet twice sets the same bit, which is what makes duplicate acks harmless.
    u32 before = peer.ack_bits;
    _nya_net_udp_record_ack(&peer, 7);
    nya_assert(peer.ack_bits == before, "a duplicate changed the bitfield");
  }

  printf("TEST: a packet older than the window is ignored\n");
  {
    _NYA_NetUdpPeer peer = { 0 };

    _nya_net_udp_record_ack(&peer, 100);

    u32 before = peer.ack_bits;

    // Exactly at the edge is inside; one past it is not. Recording it anyway would set a bit belonging to a
    // different packet, which is worse than not recording it at all.
    _nya_net_udp_record_ack(&peer, 100 - ACK_WINDOW);
    nya_assert((peer.ack_bits & (1U << (ACK_WINDOW - 1))) != 0, "the oldest packet in the window was not recorded");

    before = peer.ack_bits;

    _nya_net_udp_record_ack(&peer, 100 - ACK_WINDOW - 1);
    nya_assert(peer.ack_bits == before, "a packet past the window changed the bitfield");

    _nya_net_udp_record_ack(&peer, 0);
    nya_assert(peer.remote_sequence == 100, "an ancient packet moved the newest sequence");
  }

  printf("TEST: a jump further than the window clears rather than over-shifting\n");
  {
    /*
     * The undefined-behaviour case.
     */
    _NYA_NetUdpPeer peer = { 0 };

    _nya_net_udp_record_ack(&peer, 1);
    _nya_net_udp_record_ack(&peer, 2);
    _nya_net_udp_record_ack(&peer, 3);

    nya_assert(peer.ack_bits != 0, "there should be history to lose");

    // Exactly the window width, which is the first shift that is undefined. Only the previous newest is still in reach.
    _nya_net_udp_record_ack(&peer, 3 + ACK_WINDOW);

    nya_assert(peer.remote_sequence == 3 + ACK_WINDOW);
    nya_assert(peer.ack_bits == 1U << (ACK_WINDOW - 1), "a jump of the full window keeps only the old newest, got %u", peer.ack_bits);

    // A much larger jump forward.
    _nya_net_udp_record_ack(&peer, 20000);
    nya_assert(peer.remote_sequence == 20000, "a large forward jump was not taken");
    nya_assert(peer.ack_bits == 0, "a large jump should clear the field");

    // One short of the window still shifts rather than clearing, so the boundary is not off by one.
    _NYA_NetUdpPeer edge = { 0 };
    _nya_net_udp_record_ack(&edge, 1);
    _nya_net_udp_record_ack(&edge, 1 + ACK_WINDOW - 1);

    nya_assert(edge.ack_bits != 0, "a jump one short of the window should keep the oldest bit");
  }

  printf("TEST: the bitfield survives the sequence wrap\n");
  {
    /*
     * Packets either side of 65535, which is the case a session reaches after 65536 packets and no
     * end-to-end test reaches at all.
     */
    _NYA_NetUdpPeer peer = { 0 };

    // what the receiver does: the 16 bits on the wire, widened against the newest sequence it has.
    for (u32 wire = 65533; wire <= 65537; wire++) {
      _nya_net_udp_record_ack(&peer, _nya_net_udp_sequence_expand(peer.remote_sequence, (u16)wire));
    }

    nya_assert(peer.remote_sequence == 65537, "the newest sequence after the wrap is 65537, got %llu", (unsigned long long)peer.remote_sequence);

    // 0 is one back from 1, 65535 is two back, and so on across the wrap.
    nya_assert((peer.ack_bits & (1U << 0)) != 0, "packet 0 should be one back from 1");
    nya_assert((peer.ack_bits & (1U << 1)) != 0, "packet 65535 should be two back from 1");
    nya_assert((peer.ack_bits & (1U << 2)) != 0, "packet 65534 should be three back");
    nya_assert((peer.ack_bits & (1U << 3)) != 0, "packet 65533 should be four back");

    // A late packet from before the wrap is still inside the window and still recorded.
    _nya_net_udp_record_ack(&peer, _nya_net_udp_sequence_expand(peer.remote_sequence, 65530));
    nya_assert((peer.ack_bits & (1U << 6)) != 0, "a late pre-wrap packet was not recorded");
  }

  printf("TEST: a wire sequence widens to the nearest whole one\n");
  {
    nya_assert(_nya_net_udp_sequence_expand(0, 1) == 1);
    nya_assert(_nya_net_udp_sequence_expand(100, 99) == 99, "a late packet stays behind");
    nya_assert(_nya_net_udp_sequence_expand(65535, 0) == 65536, "the wrap carries into the upper bits");
    nya_assert(_nya_net_udp_sequence_expand(65536, 65535) == 65535, "a late packet from before the wrap goes back");
    nya_assert(_nya_net_udp_sequence_expand(5ULL << 16, 40000) == (4ULL << 16) + 40000, "past half the space reads as older");
    nya_assert(_nya_net_udp_sequence_expand((5ULL << 16) + 60000, 10) == (6ULL << 16) + 10, "a small id after a large newest is newer");

    // every value against every nearby newest: the answer is always within half the space and matches the wire bits.
    for (u64 newest = (3ULL << 16) - 70000; newest < (3ULL << 16) + 70000; newest += 997) {
      for (u32 wire = 0; wire < 65536; wire += 251) {
        u64 whole = _nya_net_udp_sequence_expand(newest, (u16)wire);

        nya_assert((u16)whole == wire);
        nya_assert(whole + 32768 > newest && whole <= newest + 32768, "%llu widened %u to %llu", (unsigned long long)newest, wire, (unsigned long long)whole);
      }
    }
  }

  printf("TEST: replays and packets past the window are refused\n");
  {
    _NYA_NetUdpPeer peer = { 0 };

    nya_assert(_nya_net_udp_is_replay(&peer, 0), "sequence zero names no packet");
    nya_assert(!_nya_net_udp_is_replay(&peer, 1), "the first packet is fresh");

    _nya_net_udp_record_ack(&peer, 1);
    nya_assert(_nya_net_udp_is_replay(&peer, 1), "the same packet twice is a replay");

    _nya_net_udp_record_ack(&peer, 40);
    nya_assert(!_nya_net_udp_is_replay(&peer, 39), "a late packet inside the window is fresh");
    nya_assert(!_nya_net_udp_is_replay(&peer, 40 - ACK_WINDOW), "the oldest sequence inside the window is still accepted");
    nya_assert(_nya_net_udp_is_replay(&peer, 40 - ACK_WINDOW - 1), "a packet past the window is refused");

    _nya_net_udp_record_ack(&peer, 39);
    nya_assert(_nya_net_udp_is_replay(&peer, 39), "a late packet once recorded is a replay");
    nya_assert(!_nya_net_udp_is_replay(&peer, 1000), "far ahead is fresh");
  }

  printf("TEST: a full window of packets, in every order\n");
  {
    /*
     * Thirty-three consecutive packets delivered forwards, backwards, and interleaved. However they arrive,
     * the field must end up naming all of the ones inside the window.
     */
    for (u32 pattern = 0; pattern < 3; pattern++) {
      _NYA_NetUdpPeer peer = { 0 };

      u64 base = 1000;

      // Newest first, so the window is anchored and everything after is a late arrival.
      _nya_net_udp_record_ack(&peer, base + ACK_WINDOW);

      for (u32 i = 0; i < ACK_WINDOW; i++) {
        u32 offset = pattern == 0 ? i : pattern == 1 ? (ACK_WINDOW - 1 - i) : ((i * 7) % ACK_WINDOW);

        _nya_net_udp_record_ack(&peer, base + offset);
      }

      nya_assert(peer.remote_sequence == base + ACK_WINDOW, "pattern %u moved the newest sequence", pattern);

      // Every one of the 32 before the newest is named. Anything missing means an ordering the scheme
      // mishandled.
      nya_assert(peer.ack_bits == 0xFFFFFFFFU, "pattern %u left the field at %u instead of full", pattern, peer.ack_bits);
    }
  }

  // TEST: duplicate suppression
  printf("TEST: the seen window suppresses duplicates without eating fresh ids\n");
  {
    /*
     * The transport never delivers a message twice, and duplicates are ordinary: a reliable message resent
     * after a lost acknowledgement arrives intact a second time.
     */
    _NYA_NetUdpPeer peer = { 0 };

    nya_assert(!_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5), "nothing is seen in a fresh peer");

    _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5);
    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5), "a marked id is seen");

    // testing is pure. Reassembly asks twice, once per fragment and again when the last lands, so marking
    // as a side effect would make the second ask drop every fragmented message.
    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5), "asking twice changed the answer");

    // The channels are separate, so a reliable id does not shadow an unreliable one.
    nya_assert(!_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_UNRELIABLE, 5), "the channels share a window");

    // A long ascending run, which is what a real session is. None of these may be reported as a duplicate.
    _NYA_NetUdpPeer stream = { 0 };

    for (u32 id = 0; id < 5000; id++) {
      u16 message_id = (u16)id;

      nya_assert(!_nya_net_udp_is_seen(&stream, NYA_NET_CHANNEL_RELIABLE, message_id), "id %u was reported as a duplicate", id);

      _nya_net_udp_mark_seen(&stream, NYA_NET_CHANNEL_RELIABLE, message_id);

      nya_assert(_nya_net_udp_is_seen(&stream, NYA_NET_CHANNEL_RELIABLE, message_id), "id %u was not recorded", id);

      // And the recent past is still remembered, which is what suppresses a retransmit.
      if (id >= 8) {
        nya_assert(_nya_net_udp_is_seen(&stream, NYA_NET_CHANNEL_RELIABLE, (u16)(id - 8)), "id %u forgot the id eight back", id);
      }
    }
  }

  printf("TEST: an id arriving out of order does not unmark the ones after it\n");
  {
    /*
     * Receiving 5 after 10 must not erase 10's mark, or a retransmit of 10 would be accepted again, queued
     * behind a delivery id already past it, and fill the reliable queue.
     */
    _NYA_NetUdpPeer peer = { 0 };

    _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 10);
    _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5);

    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 10), "marking 5 after 10 forgot 10");
    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 5), "5 was not recorded");
    nya_assert(!_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 7), "7 was never received");

    // Across the wrap of the u16 id space, and a jump larger than the whole window.
    _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 65530);
    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 65530), "65530 was not recorded");
    _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 3);
    nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 65530), "the wrap forgot 65530");
    nya_assert(!_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, 1), "1 was never received after the wrap");

    // Reliable ids behind the delivery point or past the reorder window are not acceptable.
    peer.next_delivery_id = 100;
    nya_assert(_nya_net_udp_reliable_acceptable(&peer, 100), "the next id is acceptable");
    nya_assert(!_nya_net_udp_reliable_acceptable(&peer, 99), "an id already delivered is not");
    nya_assert(!_nya_net_udp_reliable_acceptable(&peer, (u16)(100 + _NYA_NET_UDP_MAX_REORDER)), "an id past the window is not");
  }

  printf("TEST: retransmitted ids are suppressed\n");
  {
    _NYA_NetUdpPeer peer = { 0 };

    // a run, then the same run again, as a sender does when acknowledgements are lost.
    for (u16 id = 1; id <= 64; id++) _nya_net_udp_mark_seen(&peer, NYA_NET_CHANNEL_RELIABLE, id);

    for (u16 id = 1; id <= 64; id++) {
      nya_assert(_nya_net_udp_is_seen(&peer, NYA_NET_CHANNEL_RELIABLE, id), "id %u was not suppressed on retransmit", id);
    }
  }

  // TEST: reliable retirement
  printf("TEST: a cumulative reliable ack retires the right messages\n");
  {
    /*
     * `reliable_ack` is the next expected id, so everything older is done. Cumulative, so losing one costs
     * nothing.
     */
    NYA_Arena* arena = nya_arena_create(.name = "test_sequence");
    defer      nya_arena_destroy(arena);

    _NYA_NetUdpPeer peer = { .outgoing_reliable = nya_array_create(arena, _NYA_NetUdpReliable) };

    for (u16 id = 0; id < 10; id++) {
      u8* data = nya_arena_alloc(arena, 16);

      nya_array_push_back(peer.outgoing_reliable, ((_NYA_NetUdpReliable){ .message_id = id, .data = data, .size = 16 }));
    }

    nya_assert(peer.outgoing_reliable->length == 10);

    // Everything below 4 is delivered: 0, 1, 2, 3 go.
    _nya_net_udp_retire_reliable(&peer, arena, 4);

    nya_assert(peer.outgoing_reliable->length == 6, "retiring below 4 left %llu messages instead of 6",
               (unsigned long long)peer.outgoing_reliable->length);
    nya_assert(peer.outgoing_reliable->items[0].message_id == 4, "the wrong messages were retired");

    // The same ack again changes nothing, which is what makes a duplicate harmless.
    _nya_net_udp_retire_reliable(&peer, arena, 4);
    nya_assert(peer.outgoing_reliable->length == 6, "a repeated ack retired more");

    /*
     * An ack further ahead than anything could be outstanding is refused.
     */
    u64 before = peer.outgoing_reliable->length;

    _nya_net_udp_retire_reliable(&peer, arena, (u16)(4 + NYA_NET_MAX_RELIABLE_IN_FLIGHT + 100));

    nya_assert(peer.outgoing_reliable->length == before, "an implausible ack retired %llu messages",
               (unsigned long long)(before - peer.outgoing_reliable->length));

    // A plausible one still works, so the guard has not simply disabled retirement.
    _nya_net_udp_retire_reliable(&peer, arena, 10);
    nya_assert(peer.outgoing_reliable->length == 0, "a legitimate ack did not retire the rest");

    // And an ack against an empty queue is harmless.
    _nya_net_udp_retire_reliable(&peer, arena, 65535);
    nya_assert(peer.outgoing_reliable->length == 0);
  }

  printf("PASSED: test_sequence (0 failures)\n");

  return EXIT_SUCCESS;
}
