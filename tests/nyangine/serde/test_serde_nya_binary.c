/**
 * The binary .nya: a document with every type round trips, a typed document is refused by a build
 * with another layout, and every refusal serde_nya_binary.h promises is a case here. The round trip
 * law over generated documents is in tests/nyangine/testing/test_property.c.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BUILDING DOCUMENTS BY HAND
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The largest hand built document; the value count case is the one that needs it. */
#define DOCUMENT_BYTES_MAX (NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX + 1024)

/** Most arena a refused hostile length may cost. A header's worth of bookkeeping, nowhere near what the length claims. */
#define HOSTILE_ARENA_BYTES_MAX 4096

typedef struct {
    u8* bytes;
    u64 length;
} Document;

static void put(Document* document, u64 value, u32 width) {
    nya_assert(document->length + width <= DOCUMENT_BYTES_MAX);

    for (u32 i = 0; i < width; i++) document->bytes[document->length++] = (u8)(value >> (i * 8));
}

static void put_text(Document* document, NYA_ConstCString text, u64 length) {
    nya_assert(document->length + length <= DOCUMENT_BYTES_MAX);

    nya_memcpy(document->bytes + document->length, text, length);
    document->length += length;
}

/** A header with `flags` and `hash`, then a root object announcing `members`. */
static Document document_begin(NYA_Arena* arena, u16 flags, u64 hash, u32 members) {
    Document document = { .bytes = nya_arena_alloc(arena, DOCUMENT_BYTES_MAX) };

    put_text(&document, NYA_SERDE_NYA_BINARY_MAGIC, NYA_SERDE_NYA_BINARY_MAGIC_BYTES);
    put(&document, NYA_SERDE_NYA_BINARY_VERSION, 2);
    put(&document, flags, 2);
    put(&document, hash, 8);
    put(&document, members, 4);

    return document;
}

static void put_key(Document* document, NYA_ConstCString key) {
    put(document, strlen(key), 1);
    put_text(document, key, strlen(key));
}

/** Decodes untyped and demands a refusal whose message contains `expected`. */
static void expect_refused(NYA_Arena* arena, const Document* document, NYA_ConstCString expected) {
    NYA_Object* object = nullptr;
    NYA_Error   error  = nya_serde_nya_binary_decode(arena, document->bytes, document->length, nullptr, &object);

    nya_assert(!error.ok, "a document that should be refused (%s) decoded", expected);
    nya_assert(object == nullptr, "a refused document still handed back an object");
    nya_assert(
        strstr((NYA_ConstCString)error.message, expected) != nullptr,
        "refused for '%s', expected a message naming '%s'",
        (NYA_ConstCString)error.message,
        expected
    );
}

/** One of every value type the format has a tag for, nested once each way. */
static NYA_Object* document_with_every_type(NYA_Arena* arena) {
    NYA_Object* object = nya_object_create(arena);

    nya_object_add(object, "nothing", (NYA_Value){ .type = NYA_TYPE_NULL });
    nya_object_add(object, "b8", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
    nya_object_add(object, "b16", (NYA_Value){ .type = NYA_TYPE_B16, .as_b16 = false });
    nya_object_add(object, "b32", (NYA_Value){ .type = NYA_TYPE_B32, .as_b32 = true });
    nya_object_add(object, "b64", (NYA_Value){ .type = NYA_TYPE_B64, .as_b64 = true });
    nya_object_add(object, "b128", (NYA_Value){ .type = NYA_TYPE_B128, .as_b128 = false });
    nya_object_add(object, "u8", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = U8_MAX });
    nya_object_add(object, "u16", (NYA_Value){ .type = NYA_TYPE_U16, .as_u16 = 0xBEEF });
    nya_object_add(object, "u32", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 0xDEADBEEF });
    nya_object_add(object, "u64", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = U64_MAX });
    nya_object_add(object, "u128", (NYA_Value){ .type = NYA_TYPE_U128, .as_u128 = ((u128)U64_MAX << 64) | 7 });
    nya_object_add(object, "s8", (NYA_Value){ .type = NYA_TYPE_S8, .as_s8 = -128 });
    nya_object_add(object, "s16", (NYA_Value){ .type = NYA_TYPE_S16, .as_s16 = -2 });
    nya_object_add(object, "s32", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = -123456 });
    nya_object_add(object, "s64", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = S64_MIN });
    nya_object_add(object, "s128", (NYA_Value){ .type = NYA_TYPE_S128, .as_s128 = -((s128)1 << 100) });
    nya_object_add(object, "f16", (NYA_Value){ .type = NYA_TYPE_F16, .as_f16 = (f16)1.5F });
    nya_object_add(object, "f32", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = -0.1F });
    nya_object_add(object, "f64", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 1.0e300 });
    nya_object_add(object, "f128", (NYA_Value){ .type = NYA_TYPE_F128, .as_f128 = 1.0L / 3.0L });
    nya_object_add(object, "char", (NYA_Value){ .type = NYA_TYPE_CHAR, .as_char = 'n' });
    nya_object_add(object, "string", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine \"quoted\"\n" });
    nya_object_add(object, "empty", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "" });

    NYA_Object* inner = nya_object_create(arena);
    nya_object_add(inner, "depth", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 2 });
    nya_object_add(object, "inner", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *inner });
    nya_object_add(object, "hollow", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nya_object_create(arena) });

    NYA_ArrayᐸNYA_Valueᐳ* numbers = nya_array_create(arena, NYA_Value);
    for (u32 i = 0; i < 3; i++) nya_array_push_back(numbers, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = i * 1000 }));
    nya_object_add(object, "numbers", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *numbers });

    NYA_ArrayᐸNYA_Valueᐳ* mixed = nya_array_create(arena, NYA_Value);
    nya_array_push_back(mixed, ((NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = 1 }));
    nya_array_push_back(mixed, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "two" }));
    nya_array_push_back(mixed, ((NYA_Value){ .type = NYA_TYPE_NULL }));
    nya_object_add(object, "mixed", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *mixed });

    NYA_ArrayᐸNYA_Valueᐳ* nulls = nya_array_create(arena, NYA_Value);
    nya_array_push_back(nulls, ((NYA_Value){ .type = NYA_TYPE_NULL }));
    nya_array_push_back(nulls, ((NYA_Value){ .type = NYA_TYPE_NULL }));
    nya_object_add(object, "nulls", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *nulls });

    NYA_ArrayᐸNYA_Valueᐳ* grid = nya_array_create(arena, NYA_Value);
    nya_array_push_back(grid, ((NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *numbers }));
    nya_array_push_back(grid, ((NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = nya_array_create_on_stack(arena, NYA_Value) }));
    nya_object_add(object, "grid", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *grid });

    return object;
}

/** `levels` arrays, each the only element of the one around it, under one root key. */
static Document document_nested(NYA_Arena* arena, u32 levels) {
    Document document = document_begin(arena, 0, 0, 1);
    put_key(&document, "a");
    put(&document, 0x17, 1);

    for (u32 i = 1; i < levels; i++) {
        put(&document, 1, 4);
        put(&document, 0x17, 1);
    }
    put(&document, 0, 4);

    return document;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_serde_nya_binary");
    defer      nya_arena_destroy(arena);

    printf("TEST: every type round trips, and the bytes are the one encoding of the object\n");
    {
        NYA_Object* original = document_with_every_type(arena);

        NYA_String* bytes = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, original, nullptr, &bytes));

        NYA_Object* decoded = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_decode(arena, bytes->items, bytes->length, nullptr, &decoded));
        nya_assert(decoded->length == original->length);

        nya_assert(nya_object_get(decoded, "u64")->as_u64 == U64_MAX);
        nya_assert(nya_object_get(decoded, "u128")->as_u128 == (((u128)U64_MAX << 64) | 7));
        nya_assert(nya_object_get(decoded, "s64")->as_s64 == S64_MIN);
        nya_assert(nya_object_get(decoded, "s128")->as_s128 == -((s128)1 << 100));
        nya_assert(nya_object_get(decoded, "s8")->as_s8 == -128);
        nya_assert(nya_object_get(decoded, "f32")->as_f32 == -0.1F);
        nya_assert(nya_object_get(decoded, "f64")->as_f64 == 1.0e300);
        nya_assert(nya_object_get(decoded, "f128")->as_f128 == 1.0L / 3.0L);
        nya_assert(nya_object_get(decoded, "b32")->as_b32 == 1 && nya_object_get(decoded, "b16")->as_b16 == 0);
        nya_assert(nya_string_equals(nya_object_get(decoded, "string")->as_string, "nyangine \"quoted\"\n"));
        nya_assert(nya_object_get(decoded, "nulls")->as_array.length == 2);
        nya_assert(nya_object_get(decoded, "grid")->as_array.items[1].as_array.length == 0);

        NYA_Object* inner = &nya_object_get(decoded, "inner")->as_object;
        nya_assert(inner->length == 1 && nya_object_get(inner, "depth")->as_u32 == 2);

        NYA_String* again = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, decoded, nullptr, &again));
        nya_assert(again->length == bytes->length && nya_memcmp(again->items, bytes->items, bytes->length) == 0, "re-encoding changed the bytes");

        // and through the text form and back, which is the other half of "the same NYA_Object".
        NYA_String* text     = nya_serialize(arena, decoded, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
        NYA_Object* restored = nullptr;
        NYA_EXPECT(nya_deserialize(arena, text->items, text->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

        NYA_String* from_text = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, restored, nullptr, &from_text));
        nya_assert(
            from_text->length == bytes->length && nya_memcmp(from_text->items, bytes->items, bytes->length) == 0,
            "text lost something binary kept"
        );
        printf("  PASSED\n");
    }

    printf("TEST: the dispatch and the sniffer know the format\n");
    {
        NYA_Object* original = document_with_every_type(arena);
        NYA_String* bytes    = nya_serialize(arena, original, NYA_SERDE_FORMAT_NYA_BINARY, NYA_SERDE_PRETTY);
        nya_assert(bytes != nullptr);

        nya_assert(nya_serde_detect_format(bytes->items, bytes->length) == NYA_SERDE_FORMAT_NYA_BINARY);

        NYA_Object* back = nullptr;
        NYA_EXPECT(nya_deserialize(arena, bytes->items, bytes->length, NYA_SERDE_FORMAT_NYA_BINARY, NYA_SERDE_NONE, &back));
        nya_assert(back->length == original->length);

        // one byte short of the magic is not this format, nor any other.
        nya_assert(nya_serde_detect_format(bytes->items, NYA_SERDE_NYA_BINARY_MAGIC_BYTES - 1) == NYA_SERDE_FORMAT_COUNT);

        // a value the format has no tag for fails the serialize, it does not write something unreadable.
        NYA_Object* pointer = nya_object_create(arena);
        nya_object_add(pointer, "p", (NYA_Value){ .type = NYA_TYPE_U8_POINTER });
        nya_assert(nya_serialize(arena, pointer, NYA_SERDE_FORMAT_NYA_BINARY, NYA_SERDE_NONE) == nullptr);
        printf("  PASSED\n");
    }

    printf("TEST: a typed document is refused by a reader expecting another layout, naming both\n");
    {
        NYA_HttpAccountingDto accounting = { .enabled = true };
        NYA_Object*           document   = nya_reflect_to_object(arena, nya_reflect_of(NYA_HttpAccountingDto), &accounting);

        NYA_String* bytes = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, document, nya_reflect_of(NYA_HttpAccountingDto), &bytes));

        NYA_Object* back = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_decode(arena, bytes->items, bytes->length, nya_reflect_of(NYA_HttpAccountingDto), &back));
        nya_assert(nya_object_get(back, "enabled")->as_b8 == true);

        NYA_Error other = nya_serde_nya_binary_decode(arena, bytes->items, bytes->length, nya_reflect_of(NYA_HttpProblem), &back);
        nya_assert(
            !other.ok && strstr((NYA_ConstCString)other.message, "layout mismatch") != nullptr &&
            strstr((NYA_ConstCString)other.message, "NYA_HttpProblem") != nullptr
        );

        NYA_Error untyped = nya_serde_nya_binary_decode(arena, bytes->items, bytes->length, nullptr, &back);
        nya_assert(!untyped.ok && strstr((NYA_ConstCString)untyped.message, "expected an untyped one") != nullptr);

        NYA_String* plain = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, document, nullptr, &plain));
        NYA_Error claimed = nya_serde_nya_binary_decode(arena, plain->items, plain->length, nya_reflect_of(NYA_HttpAccountingDto), &back);
        nya_assert(!claimed.ok && strstr((NYA_ConstCString)claimed.message, "untyped") != nullptr);

        // the encoder will not tie an object to a type it does not fit.
        NYA_Object* stranger = nya_object_create(arena);
        nya_object_add(stranger, "enabled", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "yes" });
        nya_object_add(stranger, "unheard_of", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = 1 });
        NYA_String* lying = nullptr;
        nya_assert(!nya_serde_nya_binary_encode(arena, stranger, nya_reflect_of(NYA_HttpAccountingDto), &lying).ok);

        // nor will the decoder take a document that carries the right hash and the wrong shape.
        NYA_String* shaped = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, stranger, nullptr, &shaped));
        shaped->items[6] = NYA_SERDE_NYA_BINARY_FLAG_TYPED;
        u64 hash         = nya_reflect_layout_hash(nya_reflect_of(NYA_HttpAccountingDto));
        for (u32 i = 0; i < 8; i++) shaped->items[8 + i] = (u8)(hash >> (i * 8));
        NYA_Error forged = nya_serde_nya_binary_decode(arena, shaped->items, shaped->length, nya_reflect_of(NYA_HttpAccountingDto), &back);
        nya_assert(!forged.ok && strstr((NYA_ConstCString)forged.message, "does not fit") != nullptr);
        printf("  PASSED\n");
    }

    printf("TEST: the layout hash follows field names, types and offsets, and nothing else\n");
    {
        const NYA_TypeReflection* problem = nya_reflect_of(NYA_HttpProblem);

        u64 hash = nya_reflect_layout_hash(problem);
        nya_assert(hash == nya_reflect_layout_hash(problem), "the hash is a pure function");
        nya_assert(hash != nya_reflect_layout_hash(nya_reflect_of(NYA_HttpAccountingDto)));

        nya_assert(problem->field_count >= 2 && problem->field_count <= 8);
        NYA_ReflectField fields[8];
        for (u32 i = 0; i < problem->field_count; i++) fields[i] = problem->fields[i];

        NYA_TypeReflection copy = *problem;
        copy.fields             = fields;

        copy.name = "NYA_SomethingElse";
        nya_assert(nya_reflect_layout_hash(&copy) == hash, "renaming the type is not a layout change");
        copy.name = problem->name;

        fields[1].name = "renamed";
        nya_assert(nya_reflect_layout_hash(&copy) != hash, "renaming a field is");
        fields[1] = problem->fields[1];

        fields[1].offset += 1;
        nya_assert(nya_reflect_layout_hash(&copy) != hash, "moving a field is");
        fields[1] = problem->fields[1];

        fields[1].type = nya_reflect_of(u32);
        nya_assert(nya_reflect_layout_hash(&copy) != hash, "retyping a field is");
        fields[1] = problem->fields[1];

        fields[1].hint = NYA_HINT_COLOR;
        nya_assert(nya_reflect_layout_hash(&copy) == hash, "a hint is for an editor and not part of the layout");
        printf("  PASSED\n");
    }

    printf("TEST: every refusal the header promises\n");
    {
        Document empty_root = document_begin(arena, 0, 0, 0);

        // the smallest document there is, as a sanity check on the builder.
        NYA_Object* nothing = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_decode(arena, empty_root.bytes, empty_root.length, nullptr, &nothing));
        nya_assert(nothing->length == 0);

        expect_refused(arena, &(Document){ .bytes = empty_root.bytes, .length = 0 }, "empty");
        expect_refused(arena, &(Document){ .bytes = (u8*)"nya 2 1 {}", .length = 10 }, "magic");

        Document version = document_begin(arena, 0, 0, 0);
        version.bytes[4] = NYA_SERDE_NYA_BINARY_VERSION + 1;
        expect_refused(arena, &version, "version");

        expect_refused(arena, &(Document){ .bytes = empty_root.bytes, .length = 9 }, "header is truncated");
        expect_refused(arena, &(Document){ .bytes = empty_root.bytes, .length = NYA_SERDE_NYA_BINARY_HEADER_BYTES + 2 }, "member count");

        Document flags = document_begin(arena, 0x0002, 0, 0);
        expect_refused(arena, &flags, "unknown header flags");

        Document hashed = document_begin(arena, 0, 0x1234, 0);
        expect_refused(arena, &hashed, "untyped document carries a layout hash");

        Document trailing = document_begin(arena, 0, 0, 0);
        put(&trailing, 0, 1);
        expect_refused(arena, &trailing, "trailing");

        Document unknown = document_begin(arena, 0, 0, 1);
        put_key(&unknown, "a");
        put(&unknown, 0x19, 1);
        expect_refused(arena, &unknown, "unknown tag");

        // `any` names an array's elements and is not a value of its own.
        Document any = document_begin(arena, 0, 0, 1);
        put_key(&any, "a");
        put(&any, 0x18, 1);
        expect_refused(arena, &any, "unknown tag");

        Document duplicate = document_begin(arena, 0, 0, 2);
        put_key(&duplicate, "a");
        put(&duplicate, 0x00, 1);
        put_key(&duplicate, "a");
        put(&duplicate, 0x00, 1);
        expect_refused(arena, &duplicate, "appears twice");

        Document unsorted = document_begin(arena, 0, 0, 2);
        put_key(&unsorted, "b");
        put(&unsorted, 0x00, 1);
        put_key(&unsorted, "a");
        put(&unsorted, 0x00, 1);
        expect_refused(arena, &unsorted, "out of order");

        Document zero_key = document_begin(arena, 0, 0, 1);
        put(&zero_key, 2, 1);
        put_text(&zero_key, "a\0", 2);
        put(&zero_key, 0x00, 1);
        expect_refused(arena, &zero_key, "zero byte");

        Document zero_string = document_begin(arena, 0, 0, 1);
        put_key(&zero_string, "a");
        put(&zero_string, 0x15, 1);
        put(&zero_string, 3, 4);
        put_text(&zero_string, "a\0b", 3);
        expect_refused(arena, &zero_string, "zero byte");

        Document boolean = document_begin(arena, 0, 0, 1);
        put_key(&boolean, "a");
        put(&boolean, 0x01, 1);
        put(&boolean, 2, 1);
        expect_refused(arena, &boolean, "neither 0 nor 1");

        Document null_elements = document_begin(arena, 0, 0, 1);
        put_key(&null_elements, "a");
        put(&null_elements, 0x17, 1);
        put(&null_elements, 2, 4);
        put(&null_elements, 0x00, 1);
        expect_refused(arena, &null_elements, "not an element tag");

        Document needless_any = document_begin(arena, 0, 0, 1);
        put_key(&needless_any, "a");
        put(&needless_any, 0x17, 1);
        put(&needless_any, 2, 4);
        put(&needless_any, 0x18, 1);
        put(&needless_any, 0x06, 1);
        put(&needless_any, 1, 1);
        put(&needless_any, 0x06, 1);
        put(&needless_any, 2, 1);
        expect_refused(arena, &needless_any, "written with that tag");

        // the root is depth one, so the deepest document holds one array fewer than the bound.
        Document too_deep = document_nested(arena, NYA_SERDE_NYA_BINARY_DEPTH_MAX);
        expect_refused(arena, &too_deep, "nesting deeper");

        Document    deepest = document_nested(arena, NYA_SERDE_NYA_BINARY_DEPTH_MAX - 1);
        NYA_Object* deep    = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_decode(arena, deepest.bytes, deepest.length, nullptr, &deep));

        // one past the value allowance, in the one shape that reaches it cheaply: a long u8 array.
        Document many = document_begin(arena, 0, 0, 1);
        put_key(&many, "a");
        put(&many, 0x17, 1);
        put(&many, NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX, 4);
        put(&many, 0x06, 1);
        for (u32 i = 0; i < NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX; i++) put(&many, i, 1);
        expect_refused(arena, &many, "values a document may hold");

        u8* huge = nya_arena_alloc(arena, NYA_SERDE_NYA_BINARY_SIZE_MAX + 1);
        nya_memcpy(huge, empty_root.bytes, empty_root.length);
        expect_refused(arena, &(Document){ .bytes = huge, .length = NYA_SERDE_NYA_BINARY_SIZE_MAX + 1 }, "past the");

        // every proper prefix of a real document is truncated, and each is refused rather than read short.
        NYA_String* full = nullptr;
        NYA_EXPECT(nya_serde_nya_binary_encode(arena, document_with_every_type(arena), nullptr, &full));
        for (u64 cut = 0; cut < full->length; cut++) {
            NYA_Object* partial = nullptr;
            nya_assert(!nya_serde_nya_binary_decode(arena, full->items, cut, nullptr, &partial).ok, "a document cut at byte " FMTu64 " decoded", cut);
        }
        printf("  PASSED\n");
    }

    printf("TEST: a hostile length prefix is refused before anything is sized from it\n");
    {
        Document object_count = document_begin(arena, 0, 0, U32_MAX);

        Document array_count = document_begin(arena, 0, 0, 1);
        put_key(&array_count, "a");
        put(&array_count, 0x17, 1);
        put(&array_count, U32_MAX, 4);
        put(&array_count, 0x06, 1);

        Document string_length = document_begin(arena, 0, 0, 1);
        put_key(&string_length, "a");
        put(&string_length, 0x15, 1);
        put(&string_length, U32_MAX, 4);

        const Document* hostile[] = { &object_count, &array_count, &string_length };

        for (u32 i = 0; i < nya_carray_length(hostile); i++) {
            NYA_Arena* scratch = nya_arena_create(.name = "test_serde_nya_binary_hostile");
            defer      nya_arena_destroy(scratch);

            u64 before = nya_arena_stats(scratch).used_bytes;

            NYA_Object* object = nullptr;
            nya_assert(!nya_serde_nya_binary_decode(scratch, hostile[i]->bytes, hostile[i]->length, nullptr, &object).ok);

            u64 spent = nya_arena_stats(scratch).used_bytes - before;
            nya_assert(spent <= HOSTILE_ARENA_BYTES_MAX, "case %u allocated " FMTu64 " bytes for a length it refused", i, spent);
        }
        printf("  PASSED\n");
    }

    printf("TEST: the encoder refuses what it could not write back\n");
    {
        char long_key[NYA_SERDE_NYA_BINARY_KEY_BYTES_MAX + 2];
        nya_memset(long_key, 'k', sizeof(long_key) - 1);
        long_key[sizeof(long_key) - 1] = '\0';

        NYA_Object* keyed = nya_object_create(arena);
        nya_object_add(keyed, long_key, (NYA_Value){ .type = NYA_TYPE_NULL });

        NYA_String* bytes = nullptr;
        nya_assert(!nya_serde_nya_binary_encode(arena, keyed, nullptr, &bytes).ok);

        // the same depth the decoder refuses, built as an object rather than as bytes.
        NYA_Value value = { .type = NYA_TYPE_ARRAY, .as_array = nya_array_create_on_stack(arena, NYA_Value) };
        for (u32 i = 1; i < NYA_SERDE_NYA_BINARY_DEPTH_MAX; i++) {
            NYA_ArrayᐸNYA_Valueᐳ* around = nya_array_create(arena, NYA_Value);
            nya_array_push_back(around, value);
            value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *around };
        }

        NYA_Object* deep = nya_object_create(arena);
        nya_object_add(deep, "a", value);
        nya_assert(!nya_serde_nya_binary_encode(arena, deep, nullptr, &bytes).ok, "the encoder wrote a depth the decoder refuses");
        printf("  PASSED\n");
    }

    printf("PASSED: test_serde_nya_binary (0 failures)\n");
    return EXIT_SUCCESS;
}
