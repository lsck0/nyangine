/**
 * @file note_so.h
 *
 * The note as the program works with it: the SO ("system object") of "Model, SO, DTO", and the home of
 * the four conversions between the three shapes. It differs from the Model where it earns its keep —
 * `written_at_s` is the engine's own `u64` clock type here, where the stored row keeps sqlite's signed
 * INTEGER — which is the parsed-type gap the SO exists to hold. It is server only, so it carries the
 * web-profile guard and may see both the Model and the DTO; the DTO, being the shared shape, sees neither.
 *
 * The conversions are the only conversions there are for a note, each a total function named by the
 * style's from/to vocabulary. Three are total; note_so_from_dto is fallible, because a DTO off a client
 * is untrusted input and turning it into an SO is where it is checked — and where the server, not the
 * client, decides the owner and the timestamp. Nothing converts a DTO straight into a Model: the SO is
 * always in the middle, which is what keeps a client from ever writing an `owner`.
 * */
#pragma once

#include "nyangine-std/base/base_web_profile.h"

#include "nyangine-std/base/base_error.h"

#include "note_dto.h"
#include "note_model.h"

static_assert(NOTE_MODEL_TEXT_MAX == NOTE_DTO_TEXT_MAX, "the note text limit must agree across the model and the dto, or a conversion would truncate");

/** One note as the program holds it in memory. `owner` never leaves the server; the DTO has no such field. */
typedef struct {
    s64  id;
    s64  owner;
    u64  written_at_s;
    char text[NOTE_MODEL_TEXT_MAX];
} Note;

/** A stored row becomes the object the program works with. Total: every field maps. */
static inline Note note_so_from_model(const AccountNote* model) {
    Note so = {
        .id           = model->id,
        .owner        = model->owner,
        .written_at_s = (u64)model->written_at_s,
    };
    (void)snprintf(so.text, sizeof(so.text), "%s", model->text);
    return so;
}

/** The object the program works with becomes a row to store. Total: the inverse of note_so_from_model. */
static inline AccountNote note_model_from_so(const Note* so) {
    AccountNote model = {
        .id           = so->id,
        .owner        = so->owner,
        .written_at_s = (s64)so->written_at_s,
    };
    (void)snprintf(model.text, sizeof(model.text), "%s", so->text);
    return model;
}

/** The object becomes what a client may see. Total, and lossy on purpose: `owner` is dropped. */
static inline NoteDtoV1 note_dto_from_so(const Note* so) {
    NoteDtoV1 dto = {
        .id           = so->id,
        .written_at_s = (s64)so->written_at_s,
    };
    (void)snprintf(dto.text, sizeof(dto.text), "%s", so->text);
    return dto;
}

/**
 * A DTO off the wire becomes an object, or is refused. Fallible: this is the parse, so the untrusted
 * text is validated here. `owner` and `written_at_s` come from the server — the caller's authenticated
 * id and the server clock — never from the DTO, which is how a client is kept from claiming a note it
 * did not write or backdating one.
 * */
static inline NYA_Error note_so_from_dto(const NoteDtoV1* dto, s64 owner, u64 now_s, Note* out) {
    if (dto == nullptr || out == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a null note dto or destination");
    if (dto->text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a note with no text");

    *out = (Note){
        .id           = dto->id, // zero for a new note; the ORM assigns the row id on insert.
        .owner        = owner,
        .written_at_s = now_s,
    };
    (void)snprintf(out->text, sizeof(out->text), "%s", dto->text);
    return NYA_OK;
}
