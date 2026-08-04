#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Lexer nya_lexer_create(NYA_ConstCString source) {
    nya_assert(source != nullptr);

    NYA_Lexer lexer = {
        // A token stream is small and short lived, so it gets a region sized for one rather than
        // the gibibyte default, which a sanitized build pays for in full on creation.
        .arena               = nya_arena_create(.region_size = nya_mebyte_to_byte(1UL)),
        .source              = source,
        .cursor              = 0,
        .current_line_number = 1,
        .current_char_number = 1,
    };
    lexer.tokens = nya_array_create(lexer.arena, NYA_Token);

    return lexer;
}

void nya_lexer_run(NYA_Lexer* lexer) {
    nya_assert(lexer != nullptr);

    while (true) {
        u8 current_char = lexer->source[lexer->cursor];

        // end of source
        if (current_char == '\0') {
            NYA_Token token = {
                .type            = NYA_TOKEN_EOF,
                .source_location = lexer->cursor,
                .length          = 0,
                .line_number     = lexer->current_line_number,
                .char_number     = lexer->current_char_number,
            };
            nya_array_push_back(lexer->tokens, token);
            return;
        }

        // handle whitespace
        if (current_char == ' ' || current_char == '\t' || current_char == '\r') {
            lexer->cursor              += 1;
            lexer->current_char_number += 1;
            continue;
        }
        if (current_char == '\n') {
            lexer->cursor              += 1;
            lexer->current_line_number += 1;
            lexer->current_char_number  = 1;
            continue;
        }

        // lex identifier
        if (('a' <= current_char && current_char <= 'z') || ('A' <= current_char && current_char <= 'Z') || (current_char == '_')) {
            u32 start_cursor      = lexer->cursor;
            u32 start_char_number = lexer->current_char_number;
            u32 start_line_number = lexer->current_line_number;

            while (true) {
                current_char = lexer->source[lexer->cursor];
                if (!(('a' <= current_char && current_char <= 'z') || ('A' <= current_char && current_char <= 'Z') ||
                      ('0' <= current_char && current_char <= '9') || (current_char == '_'))) {
                    break;
                }
                lexer->cursor              += 1;
                lexer->current_char_number += 1;
            }

            NYA_Token token = {
                .type            = NYA_TOKEN_IDENT,
                .source_location = start_cursor,
                .length          = lexer->cursor - start_cursor,
                .line_number     = start_line_number,
                .char_number     = start_char_number,
            };
            nya_array_push_back(lexer->tokens, token);

            continue;
        }

        // lex number (decimal, hex, binary)
        if (('0' <= current_char && current_char <= '9')) {
            u32 start_cursor      = lexer->cursor;
            u32 start_char_number = lexer->current_char_number;
            u32 start_line_number = lexer->current_line_number;
            b8  is_float          = false;
            b8  is_hex            = false;
            b8  is_binary         = false;

            // check for hex (0x) or binary (0b) prefix
            if (current_char == '0' && lexer->source[lexer->cursor + 1] != '\0') {
                u8 next_char = lexer->source[lexer->cursor + 1];
                if (next_char == 'x' || next_char == 'X') {
                    is_hex                      = true;
                    lexer->cursor              += 2;
                    lexer->current_char_number += 2;
                } else if (next_char == 'b' || next_char == 'B') {
                    is_binary                   = true;
                    lexer->cursor              += 2;
                    lexer->current_char_number += 2;
                }
            }

            while (true) {
                current_char = lexer->source[lexer->cursor];
                if (is_hex) {
                    // A hexadecimal float: 0x1.91eb86p+1. The mantissa is written in hex so it
                    // survives a round trip through text exactly, which decimal cannot promise, and
                    // the dot is part of the number rather than the end of it.
                    if (current_char == '.') {
                        if (is_float) break;
                        is_float                    = true;
                        lexer->cursor              += 1;
                        lexer->current_char_number += 1;
                        continue;
                    }
                    if (!(('0' <= current_char && current_char <= '9') || ('a' <= current_char && current_char <= 'f') ||
                          ('A' <= current_char && current_char <= 'F'))) {
                        break;
                    }
                } else if (is_binary) {
                    if (!(current_char == '0' || current_char == '1')) { break; }
                } else {
                    if (current_char == '.') {
                        if (is_float) break;
                        is_float                    = true;
                        lexer->cursor              += 1;
                        lexer->current_char_number += 1;
                        continue;
                    }
                    if (!('0' <= current_char && current_char <= '9')) break;
                }
                lexer->cursor              += 1;
                lexer->current_char_number += 1;
            }

            // check if floats have at least one digit after the dot
            if (is_float) {
                u32 dot_index = start_cursor;
                while (dot_index < lexer->cursor && lexer->source[dot_index] != '.') dot_index += 1;
                u8 after_dot = dot_index + 1 < lexer->cursor ? lexer->source[dot_index + 1] : '\0';
                b8 digit_after_dot =
                    ('0' <= after_dot && after_dot <= '9') ||
                    (is_hex && (('a' <= after_dot && after_dot <= 'f') || ('A' <= after_dot && after_dot <= 'F')));

                if (dot_index + 1 >= lexer->cursor || !digit_after_dot) {
                    lexer->cursor = dot_index;
                    is_float      = false;
                }
            }

            // Scientific notation: 1e9, 2.5E-3, 6e+23. Only for decimal literals, since in a hex literal
            // 'e' is a digit and in a binary one it is not valid at all.
            //
            // The whole exponent is taken or none of it: "1e" and "1e+" are the number 1 followed by an
            // identifier, so the cursor rewinds rather than producing a number token that will not parse.
            /*
             * A hex literal takes its exponent with 'p' rather than 'e', because 'e' is a hex digit.
             * The exponent itself is decimal and scales by a power of two: 0x1.8p+1 is 3.
             *
             * Required rather than optional after a hex fraction — 0x1.8 alone is not a C hexadecimal
             * float — but accepted after a hex integer too, so 0x1p4 lexes as one number.
             */
            if (is_hex) {
                char exponent_char = lexer->source[lexer->cursor];

                if (exponent_char == 'p' || exponent_char == 'P') {
                    u32 exponent_cursor = lexer->cursor + 1;

                    if (lexer->source[exponent_cursor] == '+' || lexer->source[exponent_cursor] == '-') exponent_cursor += 1;

                    u32 first_digit = exponent_cursor;
                    while ('0' <= lexer->source[exponent_cursor] && lexer->source[exponent_cursor] <= '9') exponent_cursor += 1;

                    if (exponent_cursor > first_digit) {
                        lexer->current_char_number += exponent_cursor - lexer->cursor;
                        lexer->cursor               = exponent_cursor;
                        is_float                    = true;
                    }
                }
            }

            if (!is_hex && !is_binary) {
                char exponent_char = lexer->source[lexer->cursor];

                if (exponent_char == 'e' || exponent_char == 'E') {
                    u32 exponent_cursor = lexer->cursor + 1;

                    if (lexer->source[exponent_cursor] == '+' || lexer->source[exponent_cursor] == '-') exponent_cursor += 1;

                    u32 first_digit = exponent_cursor;
                    while ('0' <= lexer->source[exponent_cursor] && lexer->source[exponent_cursor] <= '9') exponent_cursor += 1;

                    if (exponent_cursor > first_digit) {
                        lexer->current_char_number += exponent_cursor - lexer->cursor;
                        lexer->cursor               = exponent_cursor;
                        is_float                    = true;
                    }
                }
            }

            NYA_Token token = {
                .type            = is_float ? NYA_TOKEN_NUMBER_FLOAT : NYA_TOKEN_NUMBER_INTEGER,
                .source_location = start_cursor,
                .length          = lexer->cursor - start_cursor,
                .line_number     = start_line_number,
                .char_number     = start_char_number,
            };
            nya_array_push_back(lexer->tokens, token);

            continue;
        }

        // lex string literal
        if (current_char == '"') {
            u32 start_cursor      = lexer->cursor + 1;
            u32 start_char_number = lexer->current_char_number;
            u32 start_line_number = lexer->current_line_number;

            lexer->cursor              += 1;
            lexer->current_char_number += 1;

            while (true) {
                current_char = lexer->source[lexer->cursor];
                if (current_char == '\0') break;
                if (current_char == '\\' && lexer->source[lexer->cursor + 1] != '\0') {
                    lexer->cursor              += 2;
                    lexer->current_char_number += 2;
                    continue;
                }
                if (current_char == '"') {
                    lexer->cursor              += 1;
                    lexer->current_char_number += 1;
                    break;
                }
                if (current_char == '\n') {
                    lexer->cursor              += 1;
                    lexer->current_line_number += 1;
                    lexer->current_char_number  = 1;
                } else {
                    lexer->cursor              += 1;
                    lexer->current_char_number += 1;
                }
            }

            NYA_Token token = {
                .type            = NYA_TOKEN_STRING,
                .source_location = start_cursor,
                .length          = lexer->cursor - start_cursor - (current_char == '"' ? 1 : 0),
                .line_number     = start_line_number,
                .char_number     = start_char_number,
            };
            nya_array_push_back(lexer->tokens, token);

            continue;
        }

        // lex symbol
        if (isprint(current_char)) {
            NYA_Token token = {
                .type            = NYA_TOKEN_SYMBOL,
                .source_location = lexer->cursor,
                .length          = 1,
                .line_number     = lexer->current_line_number,
                .char_number     = lexer->current_char_number,
                .symbol          = current_char,
            };
            nya_array_push_back(lexer->tokens, token);
            lexer->cursor              += 1;
            lexer->current_char_number += 1;

            continue;
        }

        // invalid character
        NYA_Token token = {
            .type            = NYA_TOKEN_INVALID,
            .source_location = lexer->cursor,
            .length          = 1,
            .line_number     = lexer->current_line_number,
            .char_number     = lexer->current_char_number,
        };
        nya_array_push_back(lexer->tokens, token);
        lexer->cursor              += 1;
        lexer->current_char_number += 1;
    }
}

void nya_lexer_destroy(NYA_Lexer* lexer) {
    nya_assert(lexer != nullptr);

    nya_array_destroy(lexer->tokens);
    nya_arena_destroy(lexer->arena);
}
