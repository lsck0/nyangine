/**
 * The laws, rather than the examples. Every _encode/_decode and _serialize/_deserialize pair round
 * trips, the containers behave like the obvious model of themselves, and the math identities hold.
 *
 * A failing law prints the shrunk input that breaks it; see testing_property.h.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

nya_derive_hset(u32);
nya_derive_ring(u32);
nya_derive_dict(u32);

/** Cases per law. Enough that a one-in-a-thousand shape shows up, quick enough to run with the suite. */
#define CASES 2000

/** The seed every law is drawn under. Fixed, so the suite is the same run every time; see the report line. */
#define SEED 0x6E79616E67696E65ULL

/** Bytes the byte-oriented laws generate at most. */
#define BYTES_MAX 256

/** Items the container laws push at most. */
#define ITEMS_MAX 64

/** How far two floats may drift and still count as equal after a round trip through a transform. */
#define TOLERANCE 1.0e-3F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENCODING LAWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** base64: decoding what was encoded gives back exactly the bytes that went in. */
static b8 law_base64_round_trips(NYA_Property* property) {
    u8  bytes[BYTES_MAX];
    u32 count = (u32)nya_property_draw_below(property, BYTES_MAX + 1);

    nya_property_draw_bytes(property, bytes, count);

    NYA_String* encoded = nya_string_create(property->allocator);
    nya_base64_encode(encoded, bytes, count);

    NYA_String* decoded = nya_string_create(property->allocator);
    nya_base64_decode(decoded, (const u8*)encoded->items, encoded->length);

    if (decoded->length != count) {
        nya_property_note(property, "%u bytes in, %llu out", count, (unsigned long long)decoded->length);
        return false;
    }

    return count == 0 || nya_memcmp(decoded->items, bytes, count) == 0;
}

/** compression: decompressing what was compressed gives back exactly the bytes that went in. */
static b8 law_compress_round_trips(NYA_Property* property) {
    u8  bytes[BYTES_MAX];
    u32 count = (u32)nya_property_draw_below(property, BYTES_MAX + 1);

    nya_property_draw_bytes(property, bytes, count);

    u64 bound      = nya_compress_bound(count);
    u8* compressed = nya_arena_alloc(property->allocator, bound);

    u64 written = nya_compress(bytes, count, compressed, bound);
    if (written == 0 && count > 0) {
        nya_property_note(property, "compressing %u bytes produced nothing", count);
        return false;
    }

    u8* restored = nya_arena_alloc(property->allocator, count + 1);

    if (!nya_decompress(compressed, written, restored, count)) {
        nya_property_note(property, "%u bytes compressed to %llu would not decompress", count, (unsigned long long)written);
        return false;
    }

    return count == 0 || nya_memcmp(restored, bytes, count) == 0;
}

/** A document drawn from the property's entropy: flat, but with every value type in it. */
static NYA_Object* draw_object(NYA_Property* property) {
    NYA_Object* object = nya_object_create(property->allocator);

    u32 fields = (u32)nya_property_draw_below(property, 12);

    for (u32 i = 0; i < fields; i++) {
        NYA_CString key = nya_property_draw_text(property, 12);

        // an empty key is not a key; serde has nothing to write it as.
        if (key[0] == '\0') continue;

        switch (nya_property_draw_u8(property) % 5) {
            case 0: nya_object_set(object, key, (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_property_draw_u64(property) }); break;
            case 1: nya_object_set(object, key, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)nya_property_draw_u64(property) }); break;
            case 2: nya_object_set(object, key, (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = nya_property_draw_bool(property, 50) }); break;
            case 3: nya_object_set(object, key, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_property_draw_text(property, 24) }); break;

            // the finite range only: a NaN is not equal to itself, so a round trip that preserved it
            // perfectly would still fail the comparison. What a NaN does to a parser is a fuzz
            // question, and tests/fuzz asks it.
            default: nya_object_set(object, key, (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)nya_property_draw_f32(property, -1.0e6F, 1.0e6F) }); break;
        }
    }

    return object;
}

/** Whether two values carry the same thing. Compared per type, since the union's spare bits are not written. */
static b8 values_match(NYA_Value a, NYA_Value b) {
    if (a.type != b.type) return false;

    switch (a.type) {
        case NYA_TYPE_U64:    return a.as_u64 == b.as_u64;
        case NYA_TYPE_S64:    return a.as_s64 == b.as_s64;
        case NYA_TYPE_B8:     return a.as_b8 == b.as_b8;
        case NYA_TYPE_STRING: return nya_string_equals(a.as_string, b.as_string);

        // a double survives the decimal text formats exactly only within the digits they print, so
        // this is the one comparison with a tolerance and it is relative.
        case NYA_TYPE_F64: {
            f64 scale = nya_max(1.0, fabs(a.as_f64));
            return fabs(a.as_f64 - b.as_f64) <= (f64)TOLERANCE * scale;
        }

        default: return false;
    }
}

/** One format's round trip, shared by the three laws below. */
static b8 serde_round_trips(NYA_Property* property, NYA_SerdeFormat format) {
    NYA_Object* original = draw_object(property);

    NYA_SerdeFlags flags = nya_property_draw_bool(property, 50) ? NYA_SERDE_PRETTY : NYA_SERDE_NONE;

    NYA_String* text = nya_serialize(property->allocator, original, format, flags);
    if (text == nullptr) {
        nya_property_note(property, "%s would not serialize an object of %llu fields", NYA_SERDE_FORMAT_NAME_MAP[format],
                          (unsigned long long)original->length);
        return false;
    }

    NYA_Object* restored = nullptr;
    NYA_Error   parsed   = nya_deserialize(property->allocator, (const u8*)text->items, text->length, format, flags, &restored);

    if (!parsed.ok || restored == nullptr) {
        nya_property_note(property, "%s would not read back what it wrote: %s", NYA_SERDE_FORMAT_NAME_MAP[format], (NYA_ConstCString)parsed.message);
        return false;
    }

    if (restored->length != original->length) {
        nya_property_note(property, "%s: %llu fields in, %llu out", NYA_SERDE_FORMAT_NAME_MAP[format], (unsigned long long)original->length,
                          (unsigned long long)restored->length);
        return false;
    }

    // the iterator hands out a pointer to the stored key, so every use below dereferences it once.
    nya_dict_foreach_key (original, slot) {
        NYA_CString key = *slot;

        NYA_Value* before = nya_dict_get(original, key);
        NYA_Value* after  = nya_dict_get(restored, key);

        if (after == nullptr) {
            nya_property_note(property, "%s dropped the field '%s'", NYA_SERDE_FORMAT_NAME_MAP[format], key);
            return false;
        }

        if (!values_match(*before, *after)) {
            nya_property_note(property, "%s changed the field '%s'", NYA_SERDE_FORMAT_NAME_MAP[format], key);
            return false;
        }
    }

    return true;
}

static b8 law_serde_nya_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_NYA);
}

static b8 law_serde_json_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_JSON);
}

static b8 law_serde_jsonc_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_JSONC);
}

/** A run of commands survives the wire: same count, same ticks, same inputs. */
static b8 law_net_command_round_trips(NYA_Property* property) {
    NYA_NetCommand sent[NYA_NET_COMMAND_REDUNDANCY] = { 0 };

    u32 count = 1 + (u32)nya_property_draw_below(property, NYA_NET_COMMAND_REDUNDANCY);
    u64 tick  = nya_property_draw_below(property, 1u << 20);

    for (u32 i = 0; i < count; i++) {
        // strictly increasing, which is what a run is; the decoder rejects anything else, and the
        // fuzz target is what checks that it does.
        tick += 1 + nya_property_draw_below(property, 4);

        sent[i] = (NYA_NetCommand){
            .tick    = tick,
            .actions = nya_property_draw_u64(property),
            .aim     = { nya_property_draw_f32(property, -1.0F, 1.0F), nya_property_draw_f32(property, -1.0F, 1.0F) },
            .analog  = nya_property_draw_f32(property, -1.0F, 1.0F),
        };
    }

    NYA_String* encoded = nya_string_create(property->allocator);
    if (!nya_net_command_encode(encoded, sent, count).ok) {
        nya_property_note(property, "a run of %u commands would not encode", count);
        return false;
    }

    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            received_count                       = 0;

    NYA_Error decoded = nya_net_command_decode((const u8*)encoded->items, encoded->length, received, &received_count);

    if (!decoded.ok) {
        nya_property_note(property, "a run of %u commands would not decode: %s", count, (NYA_ConstCString)decoded.message);
        return false;
    }

    if (received_count != count) {
        nya_property_note(property, "%u commands in, %u out", count, received_count);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        if (received[i].tick != sent[i].tick || received[i].actions != sent[i].actions) {
            nya_property_note(property, "command %u came back changed", i);
            return false;
        }
    }

    return true;
}

/** A snapshot survives the wire: same entities, same handles, same order. */
static b8 law_net_snapshot_round_trips(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, 17);

    NYA_NetEntityState states[16] = { 0 };

    u32 index = 0;

    for (u32 i = 0; i < count; i++) {
        // ascending and non-zero: that is what the encoder is given and what the decoder promises
        // back, and a decoder handed anything else is a fuzz question rather than a law.
        index += 1 + (u32)nya_property_draw_below(property, 8);

        states[i] = (NYA_NetEntityState){
            .handle   = { .index = index, .generation = 1 + (u32)nya_property_draw_below(property, 64) },
            .type     = (u32)nya_property_draw_below(property, 8),
            .flags    = nya_property_draw_u64(property),
            .position = { nya_property_draw_f32(property, -1000.0F, 1000.0F), nya_property_draw_f32(property, -1000.0F, 1000.0F),
                          nya_property_draw_f32(property, -1000.0F, 1000.0F) },
            .scale    = { 1.0F, 1.0F, 1.0F },
            .rotation = nya_quaternion_identity,
        };
    }

    NYA_NetSnapshot sent = { .tick = nya_property_draw_below(property, 1u << 20), .entities = states, .entity_count = count };

    NYA_String* encoded = nya_string_create(property->allocator);
    if (!nya_net_snapshot_encode(property->allocator, &sent, nullptr, encoded).ok) {
        nya_property_note(property, "a snapshot of %u entities would not encode", count);
        return false;
    }

    NYA_NetSnapshot received = { 0 };
    NYA_Error       decoded  = nya_net_snapshot_decode(property->allocator, (const u8*)encoded->items, encoded->length, nullptr, &received);

    if (!decoded.ok) {
        nya_property_note(property, "a snapshot of %u entities would not decode: %s", count, (NYA_ConstCString)decoded.message);
        return false;
    }

    if (received.entity_count != count || received.tick != sent.tick) {
        nya_property_note(property, "%u entities at tick %llu in, %u at %llu out", count, (unsigned long long)sent.tick, received.entity_count,
                          (unsigned long long)received.tick);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        if (received.entities[i].handle.index != states[i].handle.index || received.entities[i].handle.generation != states[i].handle.generation) {
            nya_property_note(property, "entity %u came back as a different handle", i);
            return false;
        }

        if (received.entities[i].flags != states[i].flags) {
            nya_property_note(property, "entity %u came back with different flags", i);
            return false;
        }
    }

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONTAINER LAWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** An array is a stack: what goes in with push_back comes out of pop_back in reverse. */
static b8 law_array_is_a_stack(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_Arrayᐸu32ᐳ* array = nya_array_create(property->allocator, u32);

    u32 pushed[ITEMS_MAX];

    for (u32 i = 0; i < count; i++) {
        pushed[i] = (u32)nya_property_draw_u64(property);
        nya_array_push_back(array, pushed[i]);

        if (array->length != i + 1) {
            nya_property_note(property, "pushing %u items left a length of %llu", i + 1, (unsigned long long)array->length);
            return false;
        }
    }

    for (u32 i = count; i > 0; i--) {
        if (array->items[array->length - 1] != pushed[i - 1]) {
            nya_property_note(property, "item %u came back as %u", i - 1, array->items[array->length - 1]);
            return false;
        }

        nya_array_pop_back(array);
    }

    return array->length == 0;
}

/** A dictionary remembers what it was told, and forgets what was removed. */
static b8 law_dict_remembers(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_Dictᐸu32ᐳ* dict = nya_dict_create(property->allocator, u32);

    for (u32 i = 0; i < count; i++) {
        NYA_CString key = nya_property_draw_text(property, 16);
        if (key[0] == '\0') continue;

        u32 value = (u32)nya_property_draw_u64(property);

        nya_dict_set(dict, key, value);

        u32* read = nya_dict_get(dict, key);

        if (read == nullptr || *read != value) {
            nya_property_note(property, "'%s' was set to %u and read back %s", key, value, read == nullptr ? "nothing" : "something else");
            return false;
        }

        if (nya_property_draw_bool(property, 30)) {
            nya_dict_remove(dict, key);

            if (nya_dict_contains(dict, key)) {
                nya_property_note(property, "'%s' survived being removed", key);
                return false;
            }
        }
    }

    return true;
}

/** A set holds each member once, and a removed member is gone. */
static b8 law_hset_holds_members_once(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_HSetᐸu32ᐳ* set = nya_hset_create(property->allocator, u32);

    for (u32 i = 0; i < count; i++) {
        u32 item = (u32)nya_property_draw_below(property, 64);

        u64 before = set->length;
        b8  known  = nya_hset_contains(set, item);

        nya_hset_insert(set, item);

        if (!nya_hset_contains(set, item)) {
            nya_property_note(property, "%u was inserted and is not in the set", item);
            return false;
        }

        // a set grows only when it learns something new. A length that grew on a repeat is a
        // duplicate, which is the one thing a set must not have.
        u64 expected = known ? before : before + 1;

        if (set->length != expected) {
            nya_property_note(property, "inserting %u took the length from %llu to %llu", item, (unsigned long long)before,
                              (unsigned long long)set->length);
            return false;
        }
    }

    return true;
}

/** A ring is a queue: what goes in first comes out first. */
static b8 law_ring_is_a_queue(NYA_Property* property) {
    NYA_Ringᐸu32ᐳ* ring = nya_ring_create_with_capacity(property->allocator, u32, ITEMS_MAX);

    u32 queued[ITEMS_MAX];
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    for (u32 i = 0; i < count; i++) {
        queued[i] = (u32)nya_property_draw_u64(property);
        nya_ring_push(ring, queued[i]);
    }

    if (nya_ring_length(ring) != count) {
        nya_property_note(property, "%u pushed, %llu held", count, (unsigned long long)nya_ring_length(ring));
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        u32 popped = nya_ring_pop(ring);

        if (popped != queued[i]) {
            nya_property_note(property, "item %u came out as %u instead of %u", i, popped, queued[i]);
            return false;
        }
    }

    return nya_ring_is_empty(ring);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MATH LAWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A drawn rotation. Normalised, since every identity below is about unit quaternions. */
static NYA_Quaternion draw_rotation(NYA_Property* property) {
    NYA_Quaternion quaternion = nya_quaternion_from_euler(
        nya_property_draw_f32(property, -3.14F, 3.14F),
        nya_property_draw_f32(property, -3.14F, 3.14F),
        nya_property_draw_f32(property, -3.14F, 3.14F)
    );

    return nya_quaternion_normalize(quaternion);
}

/** A unit quaternion times its own conjugate is the identity rotation. */
static b8 law_quaternion_conjugate_undoes(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);
    NYA_Quaternion undone     = nya_quaternion_multiply(quaternion, nya_quaternion_conjugate(quaternion));

    if (!nya_quaternion_approx_equals(undone, nya_quaternion_identity, TOLERANCE)) {
        nya_property_note(property, "q * conj(q) = (%f, %f, %f, %f)", (f64)undone.x, (f64)undone.y, (f64)undone.z, (f64)undone.w);
        return false;
    }

    return true;
}

/** Normalising twice is normalising once, and the result has length one. */
static b8 law_quaternion_normalize_is_idempotent(NYA_Property* property) {
    NYA_Quaternion once  = draw_rotation(property);
    NYA_Quaternion twice = nya_quaternion_normalize(once);

    if (!nya_quaternion_approx_equals(once, twice, TOLERANCE)) {
        nya_property_note(property, "normalising twice moved the rotation");
        return false;
    }

    f32 length = nya_quaternion_length(once);

    if (fabsf(length - 1.0F) > TOLERANCE) {
        nya_property_note(property, "a normalised quaternion has length %f", (f64)length);
        return false;
    }

    return true;
}

/** A rotation is rigid: it turns a vector without changing how long it is. */
static b8 law_quaternion_rotation_preserves_length(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);

    f32x3 vector = {
        nya_property_draw_f32(property, -100.0F, 100.0F),
        nya_property_draw_f32(property, -100.0F, 100.0F),
        nya_property_draw_f32(property, -100.0F, 100.0F),
    };

    f32 before = nya_vector_length(vector);
    f32 after  = nya_vector_length(nya_quaternion_rotate(quaternion, vector));

    // relative, because the absolute error of a rotation grows with the vector it turns.
    f32 allowed = TOLERANCE * nya_max(1.0F, before);

    if (fabsf(before - after) > allowed) {
        nya_property_note(property, "a rotation took a length of %f to %f", (f64)before, (f64)after);
        return false;
    }

    return true;
}

/** A rotation as a matrix is the same rotation: it turns a vector to the same place. */
static b8 law_quaternion_matrix_agrees(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);

    f32x3 vector = {
        nya_property_draw_f32(property, -10.0F, 10.0F),
        nya_property_draw_f32(property, -10.0F, 10.0F),
        nya_property_draw_f32(property, -10.0F, 10.0F),
    };

    f32x3 rotated = nya_quaternion_rotate(quaternion, vector);
    f32x3 matrixed = nya_matrix_times_vector(nya_quaternion_to_matrix3(quaternion), vector);

    f32 drift   = nya_vector_length(rotated - matrixed);
    f32 allowed = TOLERANCE * nya_max(1.0F, nya_vector_length(vector));

    if (drift > allowed) {
        nya_property_note(property, "the quaternion and its matrix disagree by %f", (f64)drift);
        return false;
    }

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE SUITE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    u32 failures = 0;

    printf("TEST: round trips\n");
    failures += nya_property_check("base64 round trips", CASES, SEED, law_base64_round_trips);
    failures += nya_property_check("compression round trips", CASES, SEED, law_compress_round_trips);
    failures += nya_property_check("serde nya round trips", CASES, SEED, law_serde_nya_round_trips);
    failures += nya_property_check("serde json round trips", CASES, SEED, law_serde_json_round_trips);
    failures += nya_property_check("serde jsonc round trips", CASES, SEED, law_serde_jsonc_round_trips);
    failures += nya_property_check("net commands round trip", CASES, SEED, law_net_command_round_trips);
    failures += nya_property_check("net snapshots round trip", CASES, SEED, law_net_snapshot_round_trips);

    printf("TEST: containers behave like the obvious model of themselves\n");
    failures += nya_property_check("an array is a stack", CASES, SEED, law_array_is_a_stack);
    failures += nya_property_check("a dictionary remembers", CASES, SEED, law_dict_remembers);
    failures += nya_property_check("a set holds members once", CASES, SEED, law_hset_holds_members_once);
    failures += nya_property_check("a ring is a queue", CASES, SEED, law_ring_is_a_queue);

    printf("TEST: math identities\n");
    failures += nya_property_check("conjugating undoes a rotation", CASES, SEED, law_quaternion_conjugate_undoes);
    failures += nya_property_check("normalising is idempotent", CASES, SEED, law_quaternion_normalize_is_idempotent);
    failures += nya_property_check("a rotation preserves length", CASES, SEED, law_quaternion_rotation_preserves_length);
    failures += nya_property_check("a rotation matrix agrees with its quaternion", CASES, SEED, law_quaternion_matrix_agrees);

    if (failures > 0) {
        printf("FAILED: test_property (%u failures)\n", failures);
        return EXIT_FAILURE;
    }

    printf("PASSED: test_property (0 failures)\n");

    return EXIT_SUCCESS;
}
