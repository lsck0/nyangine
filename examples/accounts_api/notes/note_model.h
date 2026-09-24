/**
 * @file note_model.h
 *
 * The note as it is stored: the database row the ORM builds a table from, `owner` and all. This is the
 * Model of "Model, SO, DTO" — server only, crossing the `db` boundary and nothing else. A column added
 * here never reaches a client by accident, because a field reaches the wire only when a *DTO* names it.
 *
 * The guard below is the compile-time half of that promise: the `web` profile builds with
 * -DNYA_WEB_PROFILE, and base_web_profile.h then refuses to compile, so this header — and the storage
 * layout in it — cannot be pulled into a wasm translation unit. It is the first include on purpose, so
 * the refusal fires before anything else is parsed.
 *
 * The reflection table is written by hand (the reflection pass scans only src/nyangine and src/gnyame);
 * inside the engine it would be one `// @reflect` struct with `id` marked `// @key`. That Model
 * reflection is what drives the ORM and the migrations, the way the DTO's reflection drives the wire.
 * */
#pragma once

#include "nyangine/base/base_web_profile.h"

#include "nyangine/base/base_memory.h"
#include "genyarated/reflection_engine.h"

/** Longest stored note, terminator included. The row's own limit; the DTO carries its own beside it. */
#define NOTE_MODEL_TEXT_MAX 280

/** One note and who owns it. `owner` is the account id; nothing is ever read across owners. */
typedef struct {
    s64  id;
    s64  owner;
    s64  written_at_s;
    char text[NOTE_MODEL_TEXT_MAX];
} AccountNote;

NYA_INTERNAL const NYA_TypeReflection NOTE_MODEL_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NOTE_MODEL_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NOTE_MODEL_TEXT_MAX,
};

NYA_INTERNAL const NYA_ReflectField NOTE_MODEL_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, id), .is_key = true },
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, owner) },
    { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(AccountNote, written_at_s) },
    { .name = "text", .type = &NOTE_MODEL_TEXT_ARRAY, .offset = nya_offsetof(AccountNote, text) },
};

/** The reflection the ORM builds the `notes` table from. */
NYA_INTERNAL const NYA_TypeReflection NOTE_MODEL = {
    .name        = "AccountNote",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(AccountNote),
    .alignment   = alignof(AccountNote),
    .fields      = NOTE_MODEL_FIELDS,
    .field_count = nya_carray_length(NOTE_MODEL_FIELDS),
};
