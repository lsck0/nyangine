/**
 * Serde throughput, both directions, over one representative document: nested objects, arrays, strings
 * and numbers of the shape a save file, a config and an HTTP body actually carry.
 *
 * Encode is measured as the object to bytes, decode as the bytes back to an object, for each of the
 * three formats a caller reaches through the dispatch: strict JSON, the native .nya text, and the native
 * binary. The number to read is ns per document; the document is built once and is the same for all six
 * cases, so the formats are comparable to each other.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/* Rows in the array of objects, so the document is a list rather than a single record. */
#define ROWS 32

/**
 * A document with every shape a reader has to handle: a few scalars, a nested object, and an array of
 * small objects with strings and numbers, which is what a list of entities or metrics rows looks like.
 * */
static NYA_Object* build_document(NYA_Arena* arena) {
    NYA_Object* root = nya_object_create(arena);

    nya_object_add(root, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "the representative document" });
    nya_object_add(root, "version", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 7 });
    nya_object_add(root, "ratio", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 0.61803398875 });
    nya_object_add(root, "enabled", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    NYA_Object* meta = nya_object_create(arena);
    nya_object_add(meta, "author", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine" });
    nya_object_add(meta, "created_at", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 1717171717ull });
    nya_object_add(meta, "note", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "a nested object, with a quote \" and a\ttab" });
    nya_object_add(root, "meta", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *meta });

    NYA_ArrayᐸNYA_Valueᐳ* rows = nya_array_create(arena, NYA_Value);
    for (u32 i = 0; i < ROWS; i++) {
        NYA_Object* row = nya_object_create(arena);

        char* label = nya_arena_alloc(arena, 32);
        (void)snprintf(label, 32, "row-%u", i);

        nya_object_add(row, "id", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = i });
        nya_object_add(row, "label", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = label });
        nya_object_add(row, "score", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = (f32)i * 1.5F });
        nya_object_add(row, "active", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = (i & 1u) == 0u });

        nya_array_push_back(rows, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *row }));
    }
    nya_object_add(root, "rows", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *rows });

    return root;
}

/** Encode both ways for one format, and report the byte size of the encoding so throughput is readable. */
static void bench_format(NYA_Arena* arena, const NYA_Object* document, NYA_SerdeFormat format, NYA_ConstCString label) {
    NYA_String* encoded = nya_serialize(arena, document, format, NYA_SERDE_NONE);
    nya_assert(encoded != nullptr && encoded->length > 0, "%s serialize produced nothing", label);

    // Confirm the round trip closes before timing it, so a broken document is a build-time surprise.
    NYA_Object* parsed = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (const u8*)encoded->items, encoded->length, format, NYA_SERDE_NONE, &parsed), "%s round trip", label);

    printf("  (%s document is %llu bytes)\n", label, (unsigned long long)encoded->length);

    char name[64] = { 0 };

    (void)snprintf(name, sizeof(name), "%s encode", label);
    nya_bench(name, 0, {
        NYA_Arena* scratch = nya_arena_create(.name = "serde_encode");
        NYA_String* out = nya_serialize(scratch, document, format, NYA_SERDE_NONE);
        nya_bench_keep(out->length);
        nya_arena_destroy(scratch);
    });

    (void)snprintf(name, sizeof(name), "%s decode", label);
    nya_bench(name, 0, {
        NYA_Arena*  scratch = nya_arena_create(.name = "serde_decode");
        NYA_Object* object  = nullptr;
        (void)nya_deserialize(scratch, (const u8*)encoded->items, encoded->length, format, NYA_SERDE_NONE, &object);
        nya_bench_keep(object);
        nya_arena_destroy(scratch);
    });
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_serde");
    defer      nya_arena_destroy(arena);

    NYA_Object* document = build_document(arena);

    nya_bench_begin("serde encode + decode, one representative document (per document)");

    bench_format(arena, document, NYA_SERDE_FORMAT_JSON, "json");
    bench_format(arena, document, NYA_SERDE_FORMAT_NYA, "nya text");
    bench_format(arena, document, NYA_SERDE_FORMAT_NYA_BINARY, "nya binary");

    if (nya_bench_end() != 0) return 1;

    return 0;
}
