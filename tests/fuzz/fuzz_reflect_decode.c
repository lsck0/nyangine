/**
 * The reflection binding, fed whatever. A DTO is filled from a request body by parsing the bytes into an
 * NYA_Object and then writing that object over the struct through the type's reflection — this is what
 * nya_http_request_reflect does with every typed body a route declares, so the object here is shaped by
 * a stranger.
 *
 * The parser has its own fuzz targets; this one is about the second half, nya_reflect_from_object, which
 * walks a hostile object against a real type: keys that name no field, values of the wrong kind, strings
 * far longer than the `char[N]` they load into, arrays longer or shorter than the fixed row count,
 * numbers that do not fit the field. The oracle is that the walk is total for any object — it writes what
 * it can and skips the rest, per base_reflection.h — so it never reads out of bounds, never asserts, and
 * never leaves the DTO holding a field it could not actually convert. A document it rejects is the
 * expected answer to garbage and is not a finding.
 *
 * NYA_HttpCeilingsDto is the target because it is the widest binding surface in the tree: a fixed array
 * of nested structs, each with a `char[64]` and several numbers, so nearly every branch of the walk is
 * reachable from one object.
 **/

// clang-format off
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "reflect_decode"

/** Nothing is asked of what a report finds; it exists only so the check walks with a reporter installed. */
static void count_problem(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_unused(path, found, expected);
    (*(u32*)user_data)++;
}

/** Parses the bytes as one format, and if a document comes back, checks it and binds it into a DTO. */
static void bind_as(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFormat format) {
    const NYA_TypeReflection* type = nya_reflect_of(NYA_HttpCeilingsDto);

    // The text formats carry a checksum this path does not want to enforce over hand-shaped input, which
    // is exactly the flag nya_http_request_reflect's document read passes for the native text.
    NYA_Object* document = nullptr;
    if (!nya_deserialize(arena, data, size, format, NYA_SERDE_NO_CHECKSUM, &document).ok) return;

    nya_assert(document != nullptr, "the reader reported success with nothing read");

    // The check is a pure walk over the document and must be total for any object; its count is a fact
    // about the document, not a pass or fail.
    u32 problems = 0;
    (void)nya_reflect_check(type, document, count_problem, &problems);
    nya_unused(problems);

    // The binding itself, over a zeroed DTO exactly as the request path does. It must return cleanly for
    // any object rather than reaching an assert or a bad write; the sanitizers are the oracle for the
    // memory, so a walk that runs past the fixed `rows` array is caught here rather than asserted for.
    static NYA_HttpCeilingsDto dto;
    memset(&dto, 0, sizeof(dto));
    (void)nya_reflect_from_object(type, &dto, document);
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_reflect_decode");
    defer      nya_arena_destroy(arena);

    // Every document format a typed body can arrive in, since the binding is reached through each.
    bind_as(arena, data, size, NYA_SERDE_FORMAT_JSON);
    bind_as(arena, data, size, NYA_SERDE_FORMAT_JSONC);
    bind_as(arena, data, size, NYA_SERDE_FORMAT_NYA);
    bind_as(arena, data, size, NYA_SERDE_FORMAT_NYA_BINARY);
}

#include "tests/fuzz/fuzz.h"
