/**
 * @file note_dto.h
 *
 * The note as it crosses to a client: the fields a reader may see, and nothing else. There is no
 * `owner` here — who wrote a note is the server's business, and a field reaches the wire only when a
 * DTO names it. This is the one of the three shapes ("Model, SO, DTO") the web profile compiles, so it
 * includes no server-only header: no `*_model.h`, no `*_so.h`, only the reflection types the wire needs.
 *
 * DTOs are versioned. A breaking change is a *new* DTO and a new route — `NoteDtoV2`, `POST /api/v2/...`
 * — never an edit to one a deployed client still sends, because the layout hash in binary `.nya` would
 * refuse the mismatch rather than misread it. `V1` is spelled into the type name so the next version
 * lives beside it instead of overwriting it.
 *
 * The reflection tables are written by hand: the reflection pass scans only src/nyangine and
 * src/gnyame, so a type declared in an example gets no generated table. Inside the engine this whole
 * file would be one `// @reflect` struct. That reflection is what drives the wire — nya_http_response_reflect
 * writes it out and nya_http_request_reflect reads it in — exactly as the Model's reflection drives the ORM.
 * */
#pragma once

#include "nyangine/base/base_memory.h"
#include "genyarated/reflection_engine.h"

/** Longest note text, terminator included. Kept beside the type so the DTO owns its own wire limit. */
#define NOTE_DTO_TEXT_MAX 280

/** Version 1 of the note DTO. A new field that an old client would choke on is a `NoteDtoV2`, not an edit. */
typedef struct {
    s64  id;
    s64  written_at_s;
    char text[NOTE_DTO_TEXT_MAX];
} NoteDtoV1;

NYA_INTERNAL const NYA_TypeReflection NOTE_DTO_V1_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NOTE_DTO_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NOTE_DTO_TEXT_MAX,
};

NYA_INTERNAL const NYA_ReflectField NOTE_DTO_V1_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteDtoV1, id) },
    { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteDtoV1, written_at_s) },
    { .name = "text", .type = &NOTE_DTO_V1_TEXT_ARRAY, .offset = nya_offsetof(NoteDtoV1, text) },
};

/** The reflection that drives the wire for a v1 note. */
NYA_INTERNAL const NYA_TypeReflection NOTE_DTO_V1_REFLECT = {
    .name        = "NoteDtoV1",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(NoteDtoV1),
    .alignment   = alignof(NoteDtoV1),
    .fields      = NOTE_DTO_V1_FIELDS,
    .field_count = nya_carray_length(NOTE_DTO_V1_FIELDS),
};
