/**
 * @file base_lexer.h
 *
 * Simple lexer/tokenizer. Supports identifiers, integers (decimal, hex 0x, binary 0b),
 * floats, symbols, and double-quoted string literals with escape sequences.
 *
 * Supported string escape sequences: \", \\, \n, \t, \r
 *
 * Example:
 * ```c
 * NYA_Lexer lexer = nya_lexer_create("name = \"hello world\";");
 * nya_lexer_run(&lexer);
 *
 * nya_array_foreach (lexer.tokens, token) { ... }
 *
 * nya_lexer_destroy(&lexer);
 * ```
 *
 * ## Comments
 *
 * `//` and slash-star comments lex as a single NYA_TOKEN_COMMENT. A parser that does not want them
 * skips that one token type; a parser that must *reject* them, as strict JSON does, leaves them in
 * place and lets its usual "unexpected token" path fire.
 *
 * This is not conditional. Both deserializers used to reassemble a comment from the two adjacent
 * symbol tokens it arrived as, with the same twenty lines written twice, and the lexer knowing what a
 * comment is deletes both copies rather than adding a third way to spell it.
 *
 * ## Dialects
 *
 * NYA_LEXER_UTF8_IDENTS is opt in, because it changes bytes that are currently invalid into part of
 * an identifier, and the deserializers rely on them being rejected.
 *
 * ```c
 * NYA_Lexer lexer = nya_lexer_create(source, NYA_LEXER_UTF8_IDENTS);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_TokenType  NYA_TokenType;
typedef enum NYA_LexerFlags NYA_LexerFlags;
typedef struct NYA_Token    NYA_Token;
typedef struct NYA_Lexer    NYA_Lexer;
nya_derive_array(NYA_Token);

enum NYA_TokenType {
    NYA_TOKEN_INVALID,

    NYA_TOKEN_EOF,
    NYA_TOKEN_SYMBOL,
    NYA_TOKEN_IDENT,
    NYA_TOKEN_NUMBER_INTEGER,
    NYA_TOKEN_NUMBER_FLOAT,
    NYA_TOKEN_STRING,

    /**
     * A line or block comment. `source_location` and `length` cover the *body*, not the delimiters.
     *
     * Appended rather than inserted next to the other content tokens: renumbering NYA_TOKEN_STRING
     * under an existing value would be a silent change to anything comparing a stored token type.
     * */
    NYA_TOKEN_COMMENT,

    NYA_TOKEN_COUNT,
};

/** Opt in lexer behaviour. See the dialect note at the top of this file. */
enum NYA_LexerFlags {
    NYA_LEXER_DEFAULT = 0,

    /**
     * Let bytes at or above 0x80 start and continue an identifier.
     *
     * For lexing this codebase's own source: the derived container types mangle their names with
     * non-ASCII brackets, so `NYA_ArrayᐸNYA_Valueᐳ` otherwise lexes as two identifiers with invalid
     * tokens between them. No attempt is made to validate UTF-8 or to exclude codepoints that are not
     * letters — the job here is to keep a name in one piece, not to implement UAX #31.
     * */
    NYA_LEXER_UTF8_IDENTS = 1u << 0,
};

struct NYA_Token {
    NYA_TokenType type;
    u32           source_location;
    u32           length;
    u32           line_number;
    u32           char_number;

    union {
        /** only present if type == NYA_TOKEN_SYMBOL */
        u8 symbol;

        /**
         * only present if type == NYA_TOKEN_COMMENT: true for slash-star, false for `//`.
         *
         * Worth having because the two associate with a declaration differently — a block comment
         * above one and a line comment trailing it are both common, and only the block form can span
         * lines, so `line_number` alone does not separate them.
         * */
        b8 is_block_comment;
    };

    /**
     * For NYA_TOKEN_STRING: source_location points to the first character after the
     * opening quote, and length is the raw content length (not including quotes).
     * Escape sequences (e.g. \", \\) are preserved as-is in the source and must be
     * processed when consuming the token value.
     * */
};

struct NYA_Lexer {
    NYA_Arena* arena;

    NYA_ConstCString source;
    u32              cursor;

    NYA_LexerFlags flags;

    u32 current_line_number;
    u32 current_char_number;

    /* will be filled after running */

    NYA_ArrayᐸNYA_Tokenᐳ* tokens;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Lexer nya_lexer_create(NYA_ConstCString source) __attr_overloaded;

/** The same, with opt in behaviour. See NYA_LexerFlags. */
NYA_API NYA_Lexer nya_lexer_create(NYA_ConstCString source, NYA_LexerFlags flags) __attr_overloaded;

NYA_API void nya_lexer_run(NYA_Lexer* lexer);
NYA_API void nya_lexer_destroy(NYA_Lexer* lexer);
