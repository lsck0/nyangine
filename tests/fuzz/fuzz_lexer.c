/**
 * The lexer, fed whatever. It reads the build system's own scans and every text format above it, so
 * its bounds are a boundary even though nothing hostile reaches it over a socket.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FUZZ_TARGET "lexer"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_lexer");
    defer      nya_arena_destroy(arena);

    // copied and terminated: the lexer takes a C string, and the input is not one.
    NYA_CString text = nya_arena_alloc(arena, size + 1);
    if (size > 0) nya_memcpy(text, data, size);
    text[size] = '\0';

    // the real length, which stops at the first embedded zero. Every span below is checked against
    // this rather than against the input size, or a NUL in the middle reads as a short token.
    u64 length = strlen(text);

    NYA_Lexer lexer = nya_lexer_create(text, NYA_LEXER_UTF8_IDENTS);
    nya_lexer_run(&lexer);
    defer nya_lexer_destroy(&lexer);

    // every token has to name a span inside the source it came from. A token reaching past it is how
    // a scan starts reading somebody else's memory.
    nya_array_foreach (lexer.tokens, token) {
        nya_assert(token->source_location <= length, "a token starts past the end of its source");
        nya_assert(token->source_location + token->length <= length + 1, "a token reaches past the end of its source");
    }
}

#include "tests/fuzz/fuzz.h"
