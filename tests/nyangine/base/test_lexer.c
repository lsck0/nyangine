/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Whether token `index` covers exactly `expected`. The token's text is a slice of the source, not a copy. */
static b8 token_text_is(const NYA_Lexer* lexer, u32 index, NYA_ConstCString expected) {
  NYA_Token token  = lexer->tokens->items[index];
  u64       length = strlen(expected);

  return token.length == length && nya_memcmp(lexer->source + token.source_location, expected, length) == 0;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Basic lexer - mixed input
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer lexer = nya_lexer_create("a+b\nc '2134.34 = .234. asdf! __asd__a __ sad__");
  nya_lexer_run(&lexer);

  NYA_Token token0  = lexer.tokens->items[0];
  NYA_Token token1  = lexer.tokens->items[1];
  NYA_Token token2  = lexer.tokens->items[2];
  NYA_Token token3  = lexer.tokens->items[3];
  NYA_Token token4  = lexer.tokens->items[4];
  NYA_Token token5  = lexer.tokens->items[5];
  NYA_Token token6  = lexer.tokens->items[6];
  NYA_Token token7  = lexer.tokens->items[7];
  NYA_Token token8  = lexer.tokens->items[8];
  NYA_Token token9  = lexer.tokens->items[9];
  NYA_Token token10 = lexer.tokens->items[10];
  NYA_Token token11 = lexer.tokens->items[11];
  NYA_Token token12 = lexer.tokens->items[12];
  NYA_Token token13 = lexer.tokens->items[13];
  NYA_Token token14 = lexer.tokens->items[14];
  NYA_Token token15 = lexer.tokens->items[15];

  nya_assert(token0.type == NYA_TOKEN_IDENT);
  nya_assert(token0.length == 1);
  nya_assert(nya_memcmp(lexer.source + token0.source_location, "a", 1) == 0);

  nya_assert(token1.type == NYA_TOKEN_SYMBOL);
  nya_assert(token1.symbol == '+');

  nya_assert(token2.type == NYA_TOKEN_IDENT);
  nya_assert(token2.length == 1);
  nya_assert(nya_memcmp(lexer.source + token2.source_location, "b", 1) == 0);

  nya_assert(token3.type == NYA_TOKEN_IDENT);
  nya_assert(token3.length == 1);
  nya_assert(nya_memcmp(lexer.source + token3.source_location, "c", 1) == 0);

  nya_assert(token4.type == NYA_TOKEN_SYMBOL);
  nya_assert(token4.symbol == '\'');

  nya_assert(token5.type == NYA_TOKEN_NUMBER_FLOAT);
  nya_assert(token5.length == 7);
  nya_assert(nya_memcmp(lexer.source + token5.source_location, "2134.34", 7) == 0);

  nya_assert(token6.type == NYA_TOKEN_SYMBOL);
  nya_assert(token6.symbol == '=');

  nya_assert(token7.type == NYA_TOKEN_SYMBOL);
  nya_assert(token7.symbol == '.');

  nya_assert(token8.type == NYA_TOKEN_NUMBER_INTEGER);
  nya_assert(token8.length == 3);
  nya_assert(nya_memcmp(lexer.source + token8.source_location, "234", 3) == 0);

  nya_assert(token9.type == NYA_TOKEN_SYMBOL);
  nya_assert(token9.symbol == '.');

  nya_assert(token10.type == NYA_TOKEN_IDENT);
  nya_assert(token10.length == 4);
  nya_assert(nya_memcmp(lexer.source + token10.source_location, "asdf", 4) == 0);

  nya_assert(token11.type == NYA_TOKEN_SYMBOL);
  nya_assert(token11.symbol == '!');

  nya_assert(token12.type == NYA_TOKEN_IDENT);
  nya_assert(token12.length == 8);
  nya_assert(nya_memcmp(lexer.source + token12.source_location, "__asd__a", 8) == 0);

  nya_assert(token13.type == NYA_TOKEN_IDENT);
  nya_assert(token13.length == 2);
  nya_assert(nya_memcmp(lexer.source + token13.source_location, "__", 2) == 0);

  nya_assert(token14.type == NYA_TOKEN_IDENT);
  nya_assert(token14.length == 5);
  nya_assert(nya_memcmp(lexer.source + token14.source_location, "sad__", 5) == 0);

  nya_assert(token15.type == NYA_TOKEN_EOF);

  nya_lexer_destroy(&lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Empty input
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer empty_lexer = nya_lexer_create("");
  nya_lexer_run(&empty_lexer);
  nya_assert(empty_lexer.tokens->length == 1);
  nya_lexer_destroy(&empty_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Whitespace only input
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer ws_lexer = nya_lexer_create("   \t\n\n   ");
  nya_lexer_run(&ws_lexer);
  nya_assert(ws_lexer.tokens->length == 1);
  nya_lexer_destroy(&ws_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Integer numbers
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer int_lexer = nya_lexer_create("0 1 42 12345 999999");
  nya_lexer_run(&int_lexer);
  nya_assert(int_lexer.tokens->length == 6);
  for (u32 i = 0; i < int_lexer.tokens->length - 1; ++i) { nya_assert(int_lexer.tokens->items[i].type == NYA_TOKEN_NUMBER_INTEGER); }
  nya_assert(int_lexer.tokens->items[0].length == 1);
  nya_assert(nya_memcmp(int_lexer.source + int_lexer.tokens->items[0].source_location, "0", 1) == 0);
  nya_assert(int_lexer.tokens->items[4].length == 6);
  nya_assert(nya_memcmp(int_lexer.source + int_lexer.tokens->items[4].source_location, "999999", 6) == 0);
  nya_lexer_destroy(&int_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Float numbers
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer float_lexer = nya_lexer_create("0.0 1.5 3.14159 123.456");
  nya_lexer_run(&float_lexer);
  nya_assert(float_lexer.tokens->length == 5);
  for (u32 i = 0; i < float_lexer.tokens->length - 1; ++i) { nya_assert(float_lexer.tokens->items[i].type == NYA_TOKEN_NUMBER_FLOAT); }
  nya_assert(float_lexer.tokens->items[2].length == 7);
  nya_assert(nya_memcmp(float_lexer.source + float_lexer.tokens->items[2].source_location, "3.14159", 7) == 0);
  nya_lexer_destroy(&float_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Identifiers with underscores
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer ident_lexer = nya_lexer_create("foo bar_baz _private __dunder__ camelCase");
  nya_lexer_run(&ident_lexer);
  nya_assert(ident_lexer.tokens->length == 6);
  for (u32 i = 0; i < ident_lexer.tokens->length - 1; ++i) { nya_assert(ident_lexer.tokens->items[i].type == NYA_TOKEN_IDENT); }
  nya_assert(ident_lexer.tokens->items[0].length == 3);
  nya_assert(ident_lexer.tokens->items[1].length == 7);
  nya_assert(ident_lexer.tokens->items[3].length == 10);
  nya_lexer_destroy(&ident_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Various symbols
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer sym_lexer = nya_lexer_create("+ - * / = < > ! @ # $ % ^ & ( ) [ ] { } ; : , ?");
  nya_lexer_run(&sym_lexer);
  nya_assert(sym_lexer.tokens->length == 25);
  for (u32 i = 0; i < sym_lexer.tokens->length - 1; ++i) { nya_assert(sym_lexer.tokens->items[i].type == NYA_TOKEN_SYMBOL); }
  nya_assert(sym_lexer.tokens->items[0].symbol == '+');
  nya_assert(sym_lexer.tokens->items[1].symbol == '-');
  nya_assert(sym_lexer.tokens->items[2].symbol == '*');
  nya_assert(sym_lexer.tokens->items[3].symbol == '/');
  nya_lexer_destroy(&sym_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Line and character number tracking
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer line_lexer = nya_lexer_create("a\nb\nc");
  nya_lexer_run(&line_lexer);
  nya_assert(line_lexer.tokens->length == 4);
  nya_assert(line_lexer.tokens->items[0].line_number == 1);
  nya_assert(line_lexer.tokens->items[1].line_number == 2);
  nya_assert(line_lexer.tokens->items[2].line_number == 3);
  nya_lexer_destroy(&line_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Identifiers with numbers
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer alphanum_lexer = nya_lexer_create("var1 test123 a1b2c3");
  nya_lexer_run(&alphanum_lexer);
  nya_assert(alphanum_lexer.tokens->length == 4);
  for (u32 i = 0; i < alphanum_lexer.tokens->length - 1; ++i) { nya_assert(alphanum_lexer.tokens->items[i].type == NYA_TOKEN_IDENT); }
  nya_assert(alphanum_lexer.tokens->items[0].length == 4);
  nya_assert(alphanum_lexer.tokens->items[1].length == 7);
  nya_lexer_destroy(&alphanum_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mixed expressions
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer expr_lexer = nya_lexer_create("x = 10 + 3.14 * y");
  nya_lexer_run(&expr_lexer);
  nya_assert(expr_lexer.tokens->length == 8);
  nya_assert(expr_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);          // x
  nya_assert(expr_lexer.tokens->items[1].type == NYA_TOKEN_SYMBOL);         // =
  nya_assert(expr_lexer.tokens->items[2].type == NYA_TOKEN_NUMBER_INTEGER); // 10
  nya_assert(expr_lexer.tokens->items[3].type == NYA_TOKEN_SYMBOL);         // +
  nya_assert(expr_lexer.tokens->items[4].type == NYA_TOKEN_NUMBER_FLOAT);   // 3.14
  nya_assert(expr_lexer.tokens->items[5].type == NYA_TOKEN_SYMBOL);         // *
  nya_assert(expr_lexer.tokens->items[6].type == NYA_TOKEN_IDENT);          // y
  nya_lexer_destroy(&expr_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Consecutive symbols
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer consec_lexer = nya_lexer_create("++--==");
  nya_lexer_run(&consec_lexer);
  nya_assert(consec_lexer.tokens->length == 7);
  nya_assert(consec_lexer.tokens->items[0].symbol == '+');
  nya_assert(consec_lexer.tokens->items[1].symbol == '+');
  nya_assert(consec_lexer.tokens->items[2].symbol == '-');
  nya_assert(consec_lexer.tokens->items[3].symbol == '-');
  nya_assert(consec_lexer.tokens->items[4].symbol == '=');
  nya_assert(consec_lexer.tokens->items[5].symbol == '=');
  nya_lexer_destroy(&consec_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Single character input
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer single_char_lexer = nya_lexer_create("x");
  nya_lexer_run(&single_char_lexer);
  nya_assert(single_char_lexer.tokens->length == 2);
  nya_assert(single_char_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
  nya_assert(single_char_lexer.tokens->items[0].length == 1);
  nya_lexer_destroy(&single_char_lexer);

  NYA_Lexer single_digit_lexer = nya_lexer_create("5");
  nya_lexer_run(&single_digit_lexer);
  nya_assert(single_digit_lexer.tokens->length == 2);
  nya_assert(single_digit_lexer.tokens->items[0].type == NYA_TOKEN_NUMBER_INTEGER);
  nya_lexer_destroy(&single_digit_lexer);

  NYA_Lexer single_sym_lexer = nya_lexer_create("+");
  nya_lexer_run(&single_sym_lexer);
  nya_assert(single_sym_lexer.tokens->length == 2);
  nya_assert(single_sym_lexer.tokens->items[0].type == NYA_TOKEN_SYMBOL);
  nya_assert(single_sym_lexer.tokens->items[0].symbol == '+');
  nya_lexer_destroy(&single_sym_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: String literals
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer str_lexer = nya_lexer_create("x = \"hello world\"");
  nya_lexer_run(&str_lexer);
  nya_assert(str_lexer.tokens->length == 4);
  nya_assert(str_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
  nya_assert(str_lexer.tokens->items[1].type == NYA_TOKEN_SYMBOL);
  nya_assert(str_lexer.tokens->items[2].type == NYA_TOKEN_STRING);
  nya_assert(str_lexer.tokens->items[2].length == 11);
  nya_assert(nya_memcmp(str_lexer.source + str_lexer.tokens->items[2].source_location, "hello world", 11) == 0);
  nya_assert(str_lexer.tokens->items[3].type == NYA_TOKEN_EOF);
  nya_lexer_destroy(&str_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: String literal with escape sequences
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Lexer esc_lexer = nya_lexer_create("\"hello\\\"world\"");
  nya_lexer_run(&esc_lexer);
  nya_assert(esc_lexer.tokens->length == 2);
  nya_assert(esc_lexer.tokens->items[0].type == NYA_TOKEN_STRING);
  nya_assert(esc_lexer.tokens->items[0].length == 12);
  nya_assert(nya_memcmp(esc_lexer.source + esc_lexer.tokens->items[0].source_location, "hello\\\"world", 12) == 0);
  nya_lexer_destroy(&esc_lexer);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: line comments
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Lexer comment_lexer = nya_lexer_create("a // note\nb");
    nya_lexer_run(&comment_lexer);

    nya_assert(comment_lexer.tokens->length == 4);
    nya_assert(comment_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
    nya_assert(comment_lexer.tokens->items[1].type == NYA_TOKEN_COMMENT);

    // The body only: the two slashes are the delimiter and are no more part of the comment than the
    // quotes are part of a string literal.
    nya_assert(token_text_is(&comment_lexer, 1, " note"), "line comment body");
    nya_assert(!comment_lexer.tokens->items[1].is_block_comment);

    // The newline ends the comment but is not consumed by it, so the line counter still advances.
    nya_assert(comment_lexer.tokens->items[2].type == NYA_TOKEN_IDENT);
    nya_assert(comment_lexer.tokens->items[2].line_number == 2, "a line comment ate its own newline");

    nya_lexer_destroy(&comment_lexer);
  }

  // A comment with nothing in it, at the very end of the source, is the case that runs off the end.
  {
    NYA_Lexer empty_comment_lexer = nya_lexer_create("//");
    nya_lexer_run(&empty_comment_lexer);

    nya_assert(empty_comment_lexer.tokens->length == 2);
    nya_assert(empty_comment_lexer.tokens->items[0].type == NYA_TOKEN_COMMENT);
    nya_assert(empty_comment_lexer.tokens->items[0].length == 0);
    nya_assert(empty_comment_lexer.tokens->items[1].type == NYA_TOKEN_EOF);

    nya_lexer_destroy(&empty_comment_lexer);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: block comments
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Lexer block_lexer = nya_lexer_create("a /* note */ b");
    nya_lexer_run(&block_lexer);

    nya_assert(block_lexer.tokens->length == 4);
    nya_assert(block_lexer.tokens->items[1].type == NYA_TOKEN_COMMENT);
    nya_assert(token_text_is(&block_lexer, 1, " note "), "block comment body");
    nya_assert(block_lexer.tokens->items[1].is_block_comment);
    nya_assert(block_lexer.tokens->items[2].type == NYA_TOKEN_IDENT);

    nya_lexer_destroy(&block_lexer);
  }

  // A block comment is the only token that can span lines, so it is the only one that can put the
  // line counter wrong for everything after it.
  {
    NYA_Lexer multiline_lexer = nya_lexer_create("a /* one\ntwo */ b");
    nya_lexer_run(&multiline_lexer);

    nya_assert(multiline_lexer.tokens->items[1].type == NYA_TOKEN_COMMENT);
    nya_assert(multiline_lexer.tokens->items[1].line_number == 1, "the comment starts where it was opened");
    nya_assert(token_text_is(&multiline_lexer, 1, " one\ntwo "), "multi-line block body");
    nya_assert(multiline_lexer.tokens->items[2].line_number == 2, "the newline inside the comment was not counted");

    nya_lexer_destroy(&multiline_lexer);
  }

  // Unterminated, which a hand written header will eventually contain. The token is still emitted
  // rather than everything before it being thrown away.
  {
    NYA_Lexer unterminated_lexer = nya_lexer_create("a /* forever");
    nya_lexer_run(&unterminated_lexer);

    nya_assert(unterminated_lexer.tokens->length == 3);
    nya_assert(unterminated_lexer.tokens->items[1].type == NYA_TOKEN_COMMENT);
    nya_assert(token_text_is(&unterminated_lexer, 1, " forever"));
    nya_assert(unterminated_lexer.tokens->items[2].type == NYA_TOKEN_EOF);

    nya_lexer_destroy(&unterminated_lexer);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a slash that opens nothing is still a symbol
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Lexer divide_lexer = nya_lexer_create("a / b");
    nya_lexer_run(&divide_lexer);

    nya_assert(divide_lexer.tokens->length == 4);
    nya_assert(divide_lexer.tokens->items[1].type == NYA_TOKEN_SYMBOL);
    nya_assert(divide_lexer.tokens->items[1].symbol == '/');

    nya_lexer_destroy(&divide_lexer);
  }

  /*
   * Separated by a space, so not an opener.
   *
   * This is what the deserializers used to have to check by comparing source offsets, since they saw
   * two symbol tokens and could not otherwise tell whether anything sat between them.
   */
  {
    NYA_Lexer spaced_lexer = nya_lexer_create("a / * b");
    nya_lexer_run(&spaced_lexer);

    nya_assert(spaced_lexer.tokens->length == 5);
    nya_assert(spaced_lexer.tokens->items[1].type == NYA_TOKEN_SYMBOL);
    nya_assert(spaced_lexer.tokens->items[1].symbol == '/');
    nya_assert(spaced_lexer.tokens->items[2].type == NYA_TOKEN_SYMBOL);
    nya_assert(spaced_lexer.tokens->items[2].symbol == '*');

    nya_lexer_destroy(&spaced_lexer);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: UTF-8 identifiers, which are opt in
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A derived container type, whose name this codebase mangles with non-ASCII brackets.
    NYA_ConstCString mangled = "NYA_HMapᐸu32ˏPlayerᐳ";

    // Off by default: the name comes apart at every byte that is not ASCII.
    NYA_Lexer ascii_lexer = nya_lexer_create(mangled);
    nya_lexer_run(&ascii_lexer);

    nya_assert(ascii_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
    nya_assert(token_text_is(&ascii_lexer, 0, "NYA_HMap"), "the name should stop at the bracket");
    nya_assert(ascii_lexer.tokens->items[1].type == NYA_TOKEN_INVALID, "a non-ASCII byte is invalid by default");
    nya_assert(ascii_lexer.tokens->length > 3, "the name came apart, which is the point of the flag");

    nya_lexer_destroy(&ascii_lexer);

    // On: one identifier, whatever bytes it is made of.
    NYA_Lexer utf8_lexer = nya_lexer_create(mangled, NYA_LEXER_UTF8_IDENTS);
    nya_lexer_run(&utf8_lexer);

    nya_assert(utf8_lexer.tokens->length == 2, "a mangled type name must lex as exactly one identifier");
    nya_assert(utf8_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
    nya_assert(token_text_is(&utf8_lexer, 0, mangled), "the whole name, unsplit");
    nya_assert(utf8_lexer.tokens->items[1].type == NYA_TOKEN_EOF);

    nya_lexer_destroy(&utf8_lexer);
  }

  // A name may also begin with a non-ASCII byte, not only continue with one.
  {
    NYA_Lexer leading_lexer = nya_lexer_create("ᐸleading", NYA_LEXER_UTF8_IDENTS);
    nya_lexer_run(&leading_lexer);

    nya_assert(leading_lexer.tokens->length == 2);
    nya_assert(leading_lexer.tokens->items[0].type == NYA_TOKEN_IDENT);
    nya_assert(token_text_is(&leading_lexer, 0, "ᐸleading"));

    nya_lexer_destroy(&leading_lexer);
  }

  // Digits still cannot start one, with or without the flag: 0x must stay a number.
  {
    NYA_Lexer digit_lexer = nya_lexer_create("0x1f", NYA_LEXER_UTF8_IDENTS);
    nya_lexer_run(&digit_lexer);

    nya_assert(digit_lexer.tokens->items[0].type == NYA_TOKEN_NUMBER_INTEGER, "UTF-8 idents changed number lexing");

    nya_lexer_destroy(&digit_lexer);
  }

  return 0;
}
