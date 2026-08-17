/**
 * Input commands: what a client tells the server it is trying to do.
 *
 * A command is the one thing a client is allowed to originate, so its encoding is the narrowest and most
 * frequently exercised untrusted surface in the protocol — sent every tick by every player, decoded into a
 * fixed-size array on the server's stack.
 *
 * The cases that matter are the boundaries rather than the round trip:
 *
 * - **A count past the redundancy limit** would write past the caller's four-entry array. That array is one
 *   call away from a socket.
 * - **A bit index past 63** is an undefined shift, not a large number.
 * - **The encoder's clamp** exists so a caller can hand over its whole ring and ask for "as many as fit"
 *   rather than having to remember the limit itself.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  nya_backtrace_init();
  defer nya_backtrace_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_command");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a run round trips exactly
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a command run round trips\n");
  {
    NYA_NetCommand sent[NYA_NET_COMMAND_REDUNDANCY] = {
      { .tick = 100, .actions = 0x1, .aim = { 1.5F, -2.5F }, .analog = 0.25F },
      { .tick = 101, .actions = 0x3, .aim = { 3.0F, 4.0F }, .analog = -1.0F },
      { .tick = 102, .actions = 0x0, .aim = { 0.0F, 0.0F }, .analog = 0.0F },
      { .tick = 103, .actions = U64_MAX, .aim = { -1024.5F, 2048.25F }, .analog = 100.0F },
    };

    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_command_encode(payload, sent, NYA_NET_COMMAND_REDUNDANCY));

    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                               = 0;

    NYA_EXPECT(nya_net_command_decode(payload->items, payload->length, received, &count));

    nya_assert(count == NYA_NET_COMMAND_REDUNDANCY, "%u of %d commands came back", count, NYA_NET_COMMAND_REDUNDANCY);

    for (u32 i = 0; i < count; i++) {
      nya_assert(received[i].tick == sent[i].tick, "command %u lost its tick", i);
      nya_assert(received[i].actions == sent[i].actions, "command %u lost its actions", i);

      // Exact, because floats cross as their bit pattern rather than through a decimal form — anything less
      // than exact means a field was read or written wrongly.
      nya_assert(received[i].aim.x == sent[i].aim.x && received[i].aim.y == sent[i].aim.y, "command %u lost its aim", i);
      nya_assert(received[i].analog == sent[i].analog, "command %u lost its analog", i);
    }

    // Order is preserved, because the server takes them in sequence and de-duplicates by tick.
    nya_assert(received[0].tick < received[3].tick, "the run came back out of order");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the encoder clamps rather than refusing
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: encoding more than the redundancy limit clamps\n");
  {
    /*
     * A caller handing over its whole ring is asking for "as many as fit", and the limit is the transport's
     * business rather than something every call site has to remember.
     */
    NYA_NetCommand many[16] = { 0 };
    for (u32 i = 0; i < 16; i++) many[i] = (NYA_NetCommand){ .tick = 200 + i, .actions = i };

    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_command_encode(payload, many, 16));

    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                               = 0;

    NYA_EXPECT(nya_net_command_decode(payload->items, payload->length, received, &count));

    nya_assert(count == NYA_NET_COMMAND_REDUNDANCY, "sixteen commands should have clamped to %d, got %u", NYA_NET_COMMAND_REDUNDANCY, count);

    // The *first* ones, because clamping takes a prefix — a caller wanting the newest passes the newest.
    nya_assert(received[0].tick == 200, "the clamp did not take the run from the start");
  }

  printf("TEST: encoding nothing is refused\n");
  {
    NYA_String* payload = nya_string_create(arena);

    nya_assert(!nya_net_command_encode(payload, nullptr, 4).ok, "encoding from a null array was accepted");

    NYA_NetCommand one = { .tick = 1 };
    nya_assert(!nya_net_command_encode(payload, &one, 0).ok, "encoding zero commands was accepted");

    nya_assert(payload->length == 0, "a refused encode wrote to the buffer anyway");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the decoder refuses what would overrun the caller's array
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a count past the limit is refused before anything is written\n");
  {
    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                               = 0;

    /*
     * The case this bound exists for. `count` is a byte a peer chose, and `received` holds exactly
     * NYA_NET_COMMAND_REDUNDANCY — so a peer claiming 255 would write past the end of an array on the
     * server's stack, one call away from a socket.
     */
    for (u32 claimed = NYA_NET_COMMAND_REDUNDANCY + 1; claimed <= 255; claimed++) {
      NYA_String* payload = nya_string_create(arena);
      nya_string_push_back(payload, (u8)claimed);

      // Enough bytes behind it that only the count check can be doing the refusing.
      for (u32 i = 0; i < claimed * 28; i++) nya_string_push_back(payload, 0x00);

      nya_assert(!nya_net_command_decode(payload->items, payload->length, received, &count).ok,
                 "a run of %u was accepted", claimed);

      nya_assert(count == 0, "a refused decode reported %u commands", count);
    }
  }

  printf("TEST: a count with too little data behind it is refused\n");
  {
    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                               = 0;

    // Every shortfall from one byte to a whole command's worth. A bounds check that is off by one shows up
    // at exactly one of these.
    for (u32 provided = 0; provided < 28 * NYA_NET_COMMAND_REDUNDANCY; provided++) {
      NYA_String* payload = nya_string_create(arena);
      nya_string_push_back(payload, NYA_NET_COMMAND_REDUNDANCY);

      for (u32 i = 0; i < provided; i++) nya_string_push_back(payload, 0x00);

      b8 ok = nya_net_command_decode(payload->items, payload->length, received, &count).ok;

      nya_assert(!ok, "a run of %d with only %u bytes behind it was accepted", NYA_NET_COMMAND_REDUNDANCY, provided);
    }

    // And exactly enough is accepted, which is what says the refusals above were about the shortfall.
    NYA_String* exact = nya_string_create(arena);
    nya_string_push_back(exact, NYA_NET_COMMAND_REDUNDANCY);
    for (u32 i = 0; i < 28 * NYA_NET_COMMAND_REDUNDANCY; i++) nya_string_push_back(exact, 0x00);

    NYA_EXPECT(nya_net_command_decode(exact->items, exact->length, received, &count));
    nya_assert(count == NYA_NET_COMMAND_REDUNDANCY);
  }

  printf("TEST: an empty payload is refused\n");
  {
    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                               = 0;

    nya_assert(!nya_net_command_decode(nullptr, 0, received, &count).ok, "a null payload was accepted");
    nya_assert(!nya_net_command_decode((const u8*)"", 0, received, &count).ok, "a zero length payload was accepted");

    // A count of zero is well formed and decodes to nothing, which is different from being refused.
    u8 empty_run[1] = { 0 };
    NYA_EXPECT(nya_net_command_decode(empty_run, sizeof(empty_run), received, &count));
    nya_assert(count == 0, "a run of zero decoded to %u commands", count);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: action bits
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: action bits, including past the end of the word\n");
  {
    NYA_NetCommand command = { 0 };

    for (u32 bit = 0; bit < 64; bit++) {
      nya_assert(!nya_net_command_holds(&command, bit), "bit %u was set in a zeroed command", bit);

      nya_net_command_set(&command, bit, true);
      nya_assert(nya_net_command_holds(&command, bit), "bit %u did not set", bit);

      nya_net_command_set(&command, bit, false);
      nya_assert(!nya_net_command_holds(&command, bit), "bit %u did not clear", bit);
    }

    // Setting one bit leaves the others alone, which a mask built with the wrong shift would not.
    nya_net_command_set(&command, 7, true);
    nya_net_command_set(&command, 40, true);

    nya_assert(nya_net_command_holds(&command, 7) && nya_net_command_holds(&command, 40));
    nya_assert(!nya_net_command_holds(&command, 8) && !nya_net_command_holds(&command, 39));

    nya_assert(command.actions == ((1ULL << 7) | (1ULL << 40)), "the actions word is %llu", (unsigned long long)command.actions);

    /*
     * Past the width of the word.
     *
     * `1ULL << 64` is undefined, not zero — so this is bounded rather than left to produce whatever the
     * hardware does with an over-wide shift. A game that has run out of action bits has a real problem, so
     * setting warns rather than failing silently, and querying answers false.
     */
    u64 before = command.actions;

    nya_net_command_set(&command, 64, true);
    nya_net_command_set(&command, 1000, true);
    nya_net_command_set(&command, U32_MAX, true);

    nya_assert(command.actions == before, "a bit past the end of the word changed the actions");

    nya_assert(!nya_net_command_holds(&command, 64), "a bit past the end reported as held");
    nya_assert(!nya_net_command_holds(&command, U32_MAX));

    // Clearing an out-of-range bit is equally harmless.
    nya_net_command_set(&command, 64, false);
    nya_assert(command.actions == before);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: random payloads never overrun
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: random payloads\n");
  {
    NYA_RNG             rng     = nya_rng_create(.seed = "C0DEC0DE");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 255.0 } };

    u32 accepted = 0;

    for (u32 iteration = 0; iteration < 20000; iteration++) {
      u8  buffer[200];
      u64 size = 1 + (iteration % (sizeof(buffer) - 1));

      for (u64 i = 0; i < size; i++) buffer[i] = nya_rng_sample_u8(&rng, uniform);

      /*
       * A canary either side of the array, so an overrun is caught even where ASan's redzone would not
       * reach — a write of exactly one element past the end lands inside this frame.
       */
      struct {
        u64            guard_low;
        NYA_NetCommand commands[NYA_NET_COMMAND_REDUNDANCY];
        u64            guard_high;
      } framed = { .guard_low = 0xA5A5A5A5A5A5A5A5ULL, .guard_high = 0x5A5A5A5A5A5A5A5AULL };

      u32 count = 0;

      if (nya_net_command_decode(buffer, size, framed.commands, &count).ok) {
        accepted++;
        nya_assert(count <= NYA_NET_COMMAND_REDUNDANCY, "iteration %u reported %u commands", iteration, count);
      }

      nya_assert(framed.guard_low == 0xA5A5A5A5A5A5A5A5ULL, "iteration %u wrote before the array", iteration);
      nya_assert(framed.guard_high == 0x5A5A5A5A5A5A5A5AULL, "iteration %u wrote past the array", iteration);
    }

    printf("  20000 random payloads, %u accepted, no overruns\n", accepted);
  }

  printf("PASSED: test_command (0 failures)\n");

  return EXIT_SUCCESS;
}
