/**
 * The "Model, SO, DTO" split, both halves proven: the conversions are total and round-trip where they
 * are lossless, and the web-profile gate actually refuses a server-only header.
 *
 * The resource is examples/accounts_api/notes/, the worked example. Its three headers are included here
 * exactly as the example includes them, and the four conversions are exercised against each other:
 *   - model -> so -> model is the identity, so nothing is lost crossing the db boundary;
 *   - so -> dto drops the owner, and the DTO's reflection — the thing that drives the wire — is shown to
 *     carry no `owner`, where the Model's reflection, which drives the ORM, does;
 *   - dto -> so is fallible and fills the owner and the timestamp from the server, never from the DTO,
 *     which is what keeps a client from claiming a note it did not write.
 *
 * The gate is proven by compiling the headers with the real compiler. A `*_model.h` and a `*_so.h`
 * refuse to compile under -DNYA_WEB_PROFILE (that is the web profile), while the `*_dto.h` compiles under
 * it: the one shape that reaches the client. This is the compile-time half of what src/build/lint.c's
 * web-profile rule also enforces statically over the whole tree.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "examples/accounts_api/notes/note_so.h"

/** Compiles one header with clang and returns its exit code; the stderr is captured into `out_stderr`. */
static s32 syntax_check(NYA_Arena* arena, NYA_ConstCString header, b8 web_profile, NYA_String** out_stderr) {
    NYA_Command cmd = {
        .arena     = arena,
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program   = "clang",
        .arguments = {
            "-x", "c", "-fsyntax-only", "-std=c2y", "-DNYA_NO_SDL", "-I./", "-I./src",
            web_profile ? "-DNYA_WEB_PROFILE" : "-DNYA_NOT_THE_WEB_PROFILE",
            header,
            nullptr,
        },
    };

    NYA_EXPECT(nya_command_run(&cmd), "clang has to run for the web-profile gate to be provable");
    if (out_stderr != nullptr) *out_stderr = cmd.stderr_content;
    return cmd.exit_code;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_model_so_dto");

    // TEST: model -> so -> model is the identity. Nothing is lost across the db boundary.
    {
        AccountNote row = { .id = 7, .owner = 42, .written_at_s = 1700000000 };
        (void)snprintf(row.text, sizeof(row.text), "%s", "a note that survives the round trip");

        Note        so   = note_so_from_model(&row);
        AccountNote back = note_model_from_so(&so);

        nya_assert(so.id == 7 && so.owner == 42 && so.written_at_s == 1700000000u, "every model field reaches the so");
        nya_assert(back.id == row.id && back.owner == row.owner && back.written_at_s == row.written_at_s, "and back again unchanged");
        nya_assert(strcmp(back.text, row.text) == 0, "the text round-trips byte for byte");
    }

    // TEST: so -> dto drops the owner, and dto -> so takes the owner and the time from the server.
    {
        Note      so  = { .id = 3, .owner = 99, .written_at_s = 1700000000u };
        (void)snprintf(so.text, sizeof(so.text), "%s", "mine");

        NoteDtoV1 dto = note_dto_from_so(&so);
        nya_assert(dto.id == 3 && dto.written_at_s == 1700000000, "the dto carries what a reader may see");
        nya_assert(strcmp(dto.text, "mine") == 0, "including the text");

        // Turning a DTO from a client into an so: the owner and the timestamp are the server's, not the DTO's.
        Note      parsed = { 0 };
        NYA_Error made   = note_so_from_dto(&dto, /*owner*/ 1234, /*now_s*/ 1800000000u, &parsed);
        nya_assert(made.ok, "a well-formed dto parses");
        nya_assert(parsed.owner == 1234, "the owner is the authenticated caller, never the DTO");
        nya_assert(parsed.written_at_s == 1800000000u, "the timestamp is the server clock, never the DTO");
        nya_assert(strcmp(parsed.text, "mine") == 0, "the text is the one field the client does supply");
    }

    // TEST: note_so_from_dto is fallible, because a DTO off the wire is untrusted.
    {
        NoteDtoV1 empty  = { .id = 0 }; // no text
        Note      out    = { 0 };
        nya_assert(!note_so_from_dto(&empty, 1, 1, &out).ok, "a note with no text is refused");
        nya_assert(!note_so_from_dto(nullptr, 1, 1, &out).ok, "a null dto is refused");
        nya_assert(!note_so_from_dto(&empty, 1, 1, nullptr).ok, "a null destination is refused");
    }

    // TEST: the DTO's reflection drives the wire and has no owner; the Model's reflection, which drives the ORM, keeps it. A field reaches the wire only when a DTO names it.
    {
        NoteDtoV1 dto = { .id = 5, .written_at_s = 10 };
        (void)snprintf(dto.text, sizeof(dto.text), "%s", "seen");

        NYA_Object* on_wire = nya_reflect_to_object(arena, &NOTE_DTO_V1_REFLECT, &dto);
        nya_assert(nya_object_get(on_wire, "id") != nullptr && nya_object_get(on_wire, "text") != nullptr, "the dto carries id and text");
        nya_assert(nya_object_get(on_wire, "owner") == nullptr, "the dto has no owner: it never reaches the wire");

        AccountNote row = { .id = 5, .owner = 77, .written_at_s = 10 };
        (void)snprintf(row.text, sizeof(row.text), "%s", "seen");

        NYA_Object* stored = nya_reflect_to_object(arena, &NOTE_MODEL, &row);
        nya_assert(nya_object_get(stored, "owner") != nullptr, "the model keeps the owner: that is what the db stores");
    }

    // TEST: the web-profile gate refuses a server-only header, and only a server-only header.
    {
        NYA_ConstCString model = "examples/accounts_api/notes/note_model.h";
        NYA_ConstCString so    = "examples/accounts_api/notes/note_so.h";
        NYA_ConstCString dto   = "examples/accounts_api/notes/note_dto.h";

        // Off the profile, every one of the three compiles: they are honest headers, not broken ones.
        nya_assert(syntax_check(arena, model, false, nullptr) == 0, "the model header compiles outside the web profile");
        nya_assert(syntax_check(arena, so, false, nullptr) == 0, "the so header compiles outside the web profile");
        nya_assert(syntax_check(arena, dto, false, nullptr) == 0, "the dto header compiles outside the web profile");

        // Under -DNYA_WEB_PROFILE the model and the so refuse to compile: this is the gate.
        NYA_String* model_stderr = nullptr;
        nya_assert(syntax_check(arena, model, true, &model_stderr) != 0, "a web TU including a *_model.h is refused");
        nya_assert(model_stderr != nullptr && nya_string_contains(model_stderr, "web profile"), "and says why");
        nya_assert(syntax_check(arena, so, true, nullptr) != 0, "a web TU including a *_so.h is refused");

        // The dto still compiles under the profile: it is the one shape the web client is sent.
        nya_assert(syntax_check(arena, dto, true, nullptr) == 0, "the dto header compiles in the web profile");
    }

    nya_arena_destroy(arena);

    printf("PASSED: model, so, dto\n");
    return EXIT_SUCCESS;
}
