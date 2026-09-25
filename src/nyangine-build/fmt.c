/**
 * @file fmt.c
 *
 * `./build fmt <file.nya>...`: validates and canonically formats `.nya` files, and with `--check` lints
 * them without rewriting.
 *
 * It dogfoods the engine's own serde rather than carrying a second parser that could drift from it: each
 * file is read through `nya_deserialize`, and a file that does not parse is reported with the parser's
 * own message and counts as a lint failure. So `--check` is the parse gate a pre-commit hook or CI wants.
 *
 * Formatting re-serializes with NYA_SERDE_PRETTY — four-space indent, one field per line, `key: type
 * value;`, the same layout the engine writes. Two things the serializer does not carry back out:
 *
 *   - Comments. The parser discards them, so a round trip would drop every `//` and block comment. The
 *     config and manifest files (engine.nya, the plugin manifests) are mostly comment, so fmt refuses to
 *     rewrite a file that has any, rather than quietly deleting them. It still validates such a file.
 *   - The header checksum. `nya_serialize` always writes the real one, while a hand-edited file carries a
 *     placeholder `0` (NYA_SERDE_NO_CHECKSUM on load). Canonicalizing fills it in, which is why a
 *     comment-free file with a placeholder header reads as drift.
 *
 * The result: fmt is a validator for every `.nya`, and a formatter for the comment-free ones — saves and
 * generated data. Formatting a commented file with comments preserved needs a CST formatter over the
 * tree-sitter grammar under tools/tree-sitter-nya, which is a separate path from this one.
 */
#include "nyangine-build/build.h"

/* PRIVATE API DECLARATION */

typedef enum _NYA_FmtOutcome {
    _NYA_FMT_OK,       // parsed, and already canonical (check) or rewritten/left alone (write).
    _NYA_FMT_DRIFTED,  // parsed, comment-free, but not in canonical form. Only reported in check mode.
    _NYA_FMT_FAILED,   // did not parse, or could not be read or written.
} _NYA_FmtOutcome;

/** Reads `path`, validates it through the engine's serde, and formats or checks it. Returns the outcome. */
NYA_INTERNAL _NYA_FmtOutcome _nya_fmt_one(NYA_Arena* arena, NYA_ConstCString path, b8 check);

/** Whether the source lexes to at least one comment token, the precise test the parser itself would make. */
NYA_INTERNAL b8 _nya_fmt_has_comments(NYA_ConstCString source);

/* PUBLIC API IMPLEMENTATION */

void nya_fmt_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    const b8          check = command->parameters[0]->value.as_b8;
    NYA_ArgParameter* files = command->parameters[1];

    if (files->values_count == 0) {
        nya_log_panic("Name at least one .nya file to %s.", check ? "check" : "format");
    }

    NYA_Arena* arena = nya_arena_create(.name = "nya_fmt_runner");
    defer nya_arena_destroy(arena);

    u32 drifted = 0;
    u32 failed  = 0;
    for (u32 index = 0; index < files->values_count; index++) {
        switch (_nya_fmt_one(arena, files->values[index].as_string, check)) {
            case _NYA_FMT_OK:      break;
            case _NYA_FMT_DRIFTED: drifted++; break;
            case _NYA_FMT_FAILED:  failed++; break;
            default:               nya_unreachable();
        }
    }

    if (failed > 0) {
        nya_log_panic("%u file%s did not parse; see above.", failed, failed == 1 ? "" : "s");
    }

    if (check) {
        if (drifted > 0) {
            nya_log_panic("%u file%s not canonically formatted; run `./build fmt` on %s.", drifted, drifted == 1 ? " is" : "s are",
                          drifted == 1 ? "it" : "them");
        }
        nya_log_info("Format check: every named .nya file is valid.");
        return;
    }

    nya_log_info("Done: %u file%s.", files->values_count, files->values_count == 1 ? "" : "s");
}

/* PRIVATE API IMPLEMENTATION */

_NYA_FmtOutcome _nya_fmt_one(NYA_Arena* arena, NYA_ConstCString path, b8 check) {
    NYA_String* contents = nya_string_create(arena);
    NYA_Error   read     = nya_file_read(path, contents);
    if (!read.ok) {
        nya_log_error("%s: cannot read (%s).", path, (const char*)read.message);
        return _NYA_FMT_FAILED;
    }

    NYA_ConstCString source = nya_string_to_cstring(arena, contents);

    NYA_Object* object = nullptr;
    // NO_CHECKSUM: these files are hand edited and carry a placeholder header; a bad checksum is not the
    // lint here, a syntax error is. The parser's message names the line and what it expected.
    NYA_Error parsed = nya_deserialize(arena, (const u8*)source, contents->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &object);
    if (!parsed.ok) {
        nya_log_error("%s: does not parse (%s).", path, (const char*)parsed.message);
        return _NYA_FMT_FAILED;
    }

    // Reformatting drops comments, so a commented file is validated only, never rewritten or flagged as
    // drift: it parsed, that is all fmt can safely say about it.
    if (_nya_fmt_has_comments(source)) {
        if (!check) nya_log_info("%s: valid; has comments, left as is (formatting would drop them).", path);
        return _NYA_FMT_OK;
    }

    NYA_String* pretty = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    // A text file ends with one newline; the serializer stops at the closing brace, so add it here.
    nya_string_push_back(pretty, '\n');

    if (nya_string_equals(contents, pretty)) return _NYA_FMT_OK;

    if (check) {
        nya_log_error("%s is not canonically formatted.", path);
        return _NYA_FMT_DRIFTED;
    }

    // Atomic, so a crash mid-write leaves the original whole rather than a truncated file.
    NYA_Error written = nya_file_write_atomic(path, pretty);
    if (!written.ok) {
        nya_log_error("%s: cannot write (%s).", path, (const char*)written.message);
        return _NYA_FMT_FAILED;
    }

    nya_log_info("formatted %s", path);
    return _NYA_FMT_OK;
}

b8 _nya_fmt_has_comments(NYA_ConstCString source) {
    NYA_Lexer lexer = nya_lexer_create(source);
    nya_lexer_run(&lexer);
    defer nya_lexer_destroy(&lexer);

    nya_array_foreach (lexer.tokens, token) {
        if (token->type == NYA_TOKEN_COMMENT) return true;
    }
    return false;
}
