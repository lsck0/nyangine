#include "nyangine/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills a case's entropy from (seed, case index), so a case replays from its coordinates alone. */
NYA_INTERNAL void _nya_property_case_fill(NYA_Property* property, u64 seed, u32 index);

/** Replays `law` against whatever is in `property->entropy`. True when the law failed. */
NYA_INTERNAL b8 _nya_property_fails(NYA_Property* property, NYA_PropertyFn law);

/**
 * Shrinks the entropy in place, keeping only variants that still fail.
 * */
NYA_INTERNAL void _nya_property_shrink(NYA_Property* property, NYA_PropertyFn law);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_property_check(NYA_ConstCString name, u32 case_count, u64 seed, NYA_PropertyFn law) {
    nya_assert(name != nullptr && name[0] != '\0');
    nya_assert(law != nullptr);
    nya_assert(case_count > 0, "a property with no cases proves nothing");

    NYA_Arena* allocator = nya_arena_create(.name = "property");
    defer      nya_arena_destroy(allocator);

    // the struct is a kilobyte of entropy plus change, which is more than belongs on a stack shared
    // with a law that allocates.
    NYA_Property* property = nya_arena_alloc(allocator, sizeof(NYA_Property));

    *property = (NYA_Property){ .name = name, .allocator = allocator };

    for (u32 index = 0; index < case_count; index++) {
        _nya_property_case_fill(property, seed, index);

        if (!_nya_property_fails(property, law)) continue;

        printf("  FAIL: %s, case %u of %u (seed 0x%016llX)\n", name, index, case_count, (unsigned long long)seed);

        u32 before = property->entropy_length;
        _nya_property_shrink(property, law);

        // replayed once more so `note` describes the shrunk case rather than the last shrink attempt,
        // which may have been one that passed.
        (void)_nya_property_fails(property, law);

        printf("    shrunk from %u to %u bytes of input\n", before, property->entropy_length);
        if (property->note[0] != '\0') printf("    %s\n", property->note);

        return 1;
    }

    if (property->exhausted > 0) {
        // not a failure: a law that runs out of entropy still ran, it just stopped varying. Worth
        // saying, because it means NYA_PROPERTY_ENTROPY_MAX is holding the law back.
        printf("  %s: %u of %u cases drew past their entropy\n", name, property->exhausted, case_count);
    }

    return 0;
}

void nya_property_note(NYA_Property* property, NYA_ConstCString format, ...) {
    nya_assert(property != nullptr);
    nya_assert(format != nullptr);

    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(property->note, sizeof(property->note), format, arguments);
    va_end(arguments);
}

/*
 * ─────────────────────────────────────────────────────────
 * DRAWS
 * ─────────────────────────────────────────────────────────
 */

u8 nya_property_draw_u8(NYA_Property* property) {
    nya_assert(property != nullptr);

    if (property->cursor >= property->entropy_length) {
        property->cursor++;
        return 0;
    }

    return property->entropy[property->cursor++];
}

u64 nya_property_draw_u64(NYA_Property* property) {
    u64 value = 0;

    // low byte first, so zeroing a later byte in the buffer shrinks the value rather than changing it
    // arbitrarily.
    for (u32 i = 0; i < 8; i++) value |= (u64)nya_property_draw_u8(property) << (i * 8);

    return value;
}

u64 nya_property_draw_below(NYA_Property* property, u64 limit) {
    if (limit == 0) return 0;

    /*
     * One byte for a small limit and eight for a large one, rather than always eight: a length drawn
     * from one byte shrinks in one step, and lengths are what everything else hangs off.
     */
    u64 drawn = limit <= 256 ? nya_property_draw_u8(property) : nya_property_draw_u64(property);

    return drawn % limit;
}

b8 nya_property_draw_bool(NYA_Property* property, u32 percent) {
    nya_assert(percent <= 100, "a chance is a percentage, got %u", percent);

    // strictly less, so zero entropy means false and a shrunk case takes the false branch.
    return (u32)(nya_property_draw_u8(property) % 100) < percent;
}

f32 nya_property_draw_f32(NYA_Property* property, f32 low, f32 high) {
    if (!(high > low)) return low;

    u8 selector = nya_property_draw_u8(property);

    // the edges first and under small bytes, so a shrink walks toward them rather than away.
    switch (selector % 8) {
        case 0:  return 0.0F;
        case 1:  return low;
        case 2:  return high;
        default: break;
    }

    f32 unit = (f32)nya_property_draw_u64(property) / (f32)U64_MAX;
    return low + ((high - low) * unit);
}

f32 nya_property_draw_f32_any(NYA_Property* property) {
    switch (nya_property_draw_u8(property) % 8) {
        case 0: return 0.0F;
        case 1: return -0.0F;
        case 2: return 1.0F;
        case 3: return -1.0F;

        default: break;
    }

    // the bit pattern, so infinities, NaNs and denormals are drawn as often as they occur in the
    // encoding rather than never.
    u32 bits = (u32)nya_property_draw_u64(property);

    f32 value = 0.0F;
    nya_memcpy(&value, &bits, sizeof(value));

    return value;
}

void nya_property_draw_bytes(NYA_Property* property, OUT u8* out, u32 count) {
    nya_assert(property != nullptr);
    nya_assert(out != nullptr || count == 0);

    for (u32 i = 0; i < count; i++) out[i] = nya_property_draw_u8(property);
}

NYA_CString nya_property_draw_text(NYA_Property* property, u32 length_max) {
    nya_assert(property != nullptr);
    nya_assert(length_max > 0);

    u32 length = (u32)nya_property_draw_below(property, length_max + 1);

    NYA_CString text = nya_arena_alloc(property->allocator, (u64)length + 1);

    // printable ASCII: a key or a label with a control character in it is a serde question, and the
    // fuzz targets ask that one. A property test states a law, and a law needs a legal input.
    for (u32 i = 0; i < length; i++) text[i] = (char)(' ' + (nya_property_draw_u8(property) % ('~' - ' ' + 1)));

    text[length] = '\0';

    return text;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_property_case_fill(NYA_Property* property, u64 seed, u32 index) {
    /*
     * The length grows with the case index, so the early cases are small and the later ones large.
     * Starting every case at the full buffer would mean the first failure is always a large one and
     * the shrinker does all the work; starting small finds the small failures first.
     */
    u32 length = 8 + (index % (NYA_PROPERTY_ENTROPY_MAX - 8));

    for (u32 at = 0; at < length; at += 8) {
        u64 coordinate[3] = { seed, index, at };
        u64 block         = nya_siphash(coordinate, sizeof(coordinate), NYA_PROPERTY_HASH_KEY_LOW, NYA_PROPERTY_HASH_KEY_HIGH);

        u32 remaining = nya_min(8U, length - at);
        nya_memcpy(property->entropy + at, &block, remaining);
    }

    property->entropy_length = length;
}

b8 _nya_property_fails(NYA_Property* property, NYA_PropertyFn law) {
    property->cursor  = 0;
    property->note[0] = '\0';

    // every case starts from an empty arena, so a law that allocates does not grow the process over a
    // few thousand cases or a few thousand shrink replays.
    nya_arena_free_all(property->allocator);

    b8 held = law(property);

    if (property->cursor > property->entropy_length) property->exhausted++;

    return !held;
}

void _nya_property_shrink(NYA_Property* property, NYA_PropertyFn law) {
    u8  candidate[NYA_PROPERTY_ENTROPY_MAX];
    u32 attempts = 0;

    while (attempts < NYA_PROPERTY_SHRINK_MAX) {
        u32 length = property->entropy_length;
        b8  better = false;

        /*
         * Shorter first, halving the tail each time: the length of a generated structure is what
         * dominates a counterexample's size, and a law that still fails on half the input was never
         * about the second half.
         */
        for (u32 cut = length / 2; cut > 0 && !better; cut /= 2) {
            attempts++;

            nya_memcpy(candidate, property->entropy, length);

            property->entropy_length = length - cut;

            if (_nya_property_fails(property, law)) {
                better = true;
                break;
            }

            property->entropy_length = length;
            nya_memcpy(property->entropy, candidate, length);
        }

        if (better) continue;

        /* Then byte by byte: zero if that still fails, otherwise halve. */
        for (u32 i = 0; i < property->entropy_length && attempts < NYA_PROPERTY_SHRINK_MAX; i++) {
            u8 original = property->entropy[i];
            if (original == 0) continue;

            attempts++;

            property->entropy[i] = 0;
            if (_nya_property_fails(property, law)) {
                better = true;
                continue;
            }

            attempts++;

            property->entropy[i] = original / 2;
            if (_nya_property_fails(property, law)) {
                better = true;
                continue;
            }

            property->entropy[i] = original;
        }

        // a whole pass that improved nothing means this is the smallest input the moves can reach.
        if (!better) break;
    }
}

#endif // NYA_TESTING
