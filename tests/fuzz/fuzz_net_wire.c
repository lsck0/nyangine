/**
 * The snapshot and command decoders, fed whatever arrives claiming to be one.
 *
 * These two are the hottest hostile boundary in the engine: a snapshot is what a client believes
 * about the world and a command run is what a server believes about a player, and both are decoded
 * before anything about the sender has been established.
 *
 * Decoded against a real baseline as well as against none, because delta decoding reads indices out
 * of the baseline and that is where an out of range one would be used.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FUZZ_TARGET "net_wire"

/** Entities in the baseline a delta is decoded against. Enough that indices into it can be wrong. */
#define BASELINE_ENTITIES 12

/** The baseline, built once: it is the same for every input and rebuilding it per case is all cost. */
static NYA_NetSnapshot BASELINE      = { 0 };
static b8              BASELINE_MADE = false;

static void make_baseline(void) {
    if (BASELINE_MADE) return;

    static NYA_NetEntityState states[BASELINE_ENTITIES];
    static NYA_Arena*         arena = nullptr;

    arena = nya_arena_create(.name = "fuzz_net_baseline");

    for (u32 i = 0; i < BASELINE_ENTITIES; i++) {
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

    NYA_NetSnapshot sent = { .tick = 40, .entities = states, .entity_count = BASELINE_ENTITIES };

    NYA_String* encoded = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &sent, nullptr, encoded));
    NYA_EXPECT(nya_net_snapshot_decode(arena, (const u8*)encoded->items, encoded->length, nullptr, &BASELINE));

    BASELINE_MADE = true;
}

#define FUZZ_SETUP make_baseline

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_net_wire");
    defer      nya_arena_destroy(arena);

    /*
     * The peek first: it is what a client calls before deciding whether a snapshot is worth decoding,
     * so it reads a header nobody has checked yet.
     */
    u64 tick = 0, baseline_tick = 0;
    (void)nya_net_snapshot_peek(data, size, &tick, &baseline_tick);

    for (u32 with_baseline = 0; with_baseline < 2; with_baseline++) {
        NYA_NetSnapshot decoded = { 0 };

        if (!nya_net_snapshot_decode(arena, data, size, with_baseline ? &BASELINE : nullptr, &decoded).ok) continue;

        // what the decoder promises its caller, which the rest of the client then relies on without
        // checking again.
        nya_assert(decoded.entity_count <= NYA_NET_MAX_REPLICATED, "a decoded snapshot holds more entities than can be replicated");

        for (u32 i = 0; i < decoded.entity_count; i++) {
            nya_assert(decoded.entities[i].handle.index < NYA_ENTITY_MAX, "a decoded handle is out of the entity table");
            nya_assert(decoded.entities[i].handle.generation != 0, "a decoded handle has no generation");

            if (i > 0) nya_assert(decoded.entities[i - 1].handle.index < decoded.entities[i].handle.index, "a decoded snapshot is out of order");
        }
    }

    NYA_NetCommand commands[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                                = 0;

    if (nya_net_command_decode(data, size, commands, &count).ok) {
        nya_assert(count <= NYA_NET_COMMAND_REDUNDANCY, "a decoded run holds more commands than one carries");

        for (u32 i = 1; i < count; i++) nya_assert(commands[i].tick > commands[i - 1].tick, "a decoded run is out of order");
    }
}

#include "fuzz/fuzz.h"
