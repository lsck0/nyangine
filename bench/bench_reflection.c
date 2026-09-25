/**
 * The reflection-driven walk: a described struct to a document and back, which is the path the ORM, the
 * config reload and every HTTP DTO binding run on hot data.
 *
 * NYA_HttpCeilingsDto is the struct under test because it is the shape reflection is asked to walk in
 * anger: a fixed array of nested structs, each with a `char[64]` name and a handful of numbers. The walk
 * touches every field of every row, so the number is the per-field conversion cost multiplied by a real
 * fan-out rather than a single flat record.
 *
 * to_object builds a document from the struct, from_object writes a struct from a document, and the
 * redacted walk is the one every log and debug dump goes through. The layout hash is the pure walk the
 * binary format stamps into a header, measured so its cost against the conversions is on record.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/* How many of the fixed rows carry meaning, so the walk has a real fan-out to cross. */
#define LIVE_ROWS 48

static void populate(NYA_HttpCeilingsDto* dto) {
    dto->count     = LIVE_ROWS;
    dto->truncated = 0;

    for (u32 i = 0; i < LIVE_ROWS; i++) {
        NYA_HttpCeilingDto* row = &dto->rows[i];
        (void)snprintf(row->name, sizeof(row->name), "ceiling-row-%u", i);
        row->capacity = 1024 + i;
        row->live     = i * 7;
        row->fullness = (f32)row->live / (f32)row->capacity;
    }
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_reflection");
    defer      nya_arena_destroy(arena);

    const NYA_TypeReflection* type = nya_reflect_of(NYA_HttpCeilingsDto);

    static NYA_HttpCeilingsDto source;
    populate(&source);

    // One document to read back from, built once and shared by the decode cases below.
    NYA_Object* document = nya_reflect_to_object(arena, type, &source);
    nya_assert(document != nullptr, "reflect_to_object produced nothing");

    nya_bench_begin("reflection walk over NYA_HttpCeilingsDto (48 nested rows, per struct)");

    // Struct to document: what a metrics answer and a save write.
    nya_bench("to_object", 0, {
        NYA_Arena*  scratch = nya_arena_create(.name = "reflect_to");
        NYA_Object* object  = nya_reflect_to_object(scratch, type, &source);
        nya_bench_keep(object);
        nya_arena_destroy(scratch);
    });

    // The redacted walk, which every log and dump path takes; this struct has no @redact field, so this
    // is the cost of the walk deciding that for every field it crosses.
    nya_bench("to_object_redacted", 0, {
        NYA_Arena*  scratch = nya_arena_create(.name = "reflect_redact");
        NYA_Object* object  = nya_reflect_to_object_redacted(scratch, type, &source);
        nya_bench_keep(object);
        nya_arena_destroy(scratch);
    });

    // Document to struct, in place: what a config reload and a DTO request binding run.
    nya_bench("from_object", 0, {
        static NYA_HttpCeilingsDto target;
        NYA_EXPECT(nya_reflect_from_object(type, &target, document));
        nya_bench_keep(target.count);
    });

    // The check that runs before a hand-edited document is applied, walking every key against the type.
    nya_bench("check", 0, {
        u32 problems = nya_reflect_check(type, document, nullptr, nullptr);
        nya_bench_keep(problems);
    });

    // The pure structural walk the binary header carries, allocation free.
    nya_bench("layout_hash", 0, {
        u64 hash = nya_reflect_layout_hash(type);
        nya_bench_keep(hash);
    });

    return nya_bench_end();
}
