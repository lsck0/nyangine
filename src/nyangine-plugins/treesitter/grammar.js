/**
 * tree-sitter grammar for the nyangine `.nya` object format.
 *
 * The shape is fixed by the engine's own parser, src/nyangine/serde/serde_nya.c, and its
 * lexer, src/nyangine/base/base_lexer.c. This grammar mirrors what that parser accepts so a
 * `.nya` file it reads has no ERROR node here, and one it rejects tends to as well.
 *
 * A document is a one line header `nya <version> <checksum>` followed by a single root object.
 * Inside, every field is `key: <type> value;` (the trailing `;` is optional). A value names its
 * type: a scalar `s32 5`, a string `string "x"`, a bool `b8 true`, a nested `object { ... }`
 * (the `object` keyword is optional before a brace), the literal `null`, or a typed array written
 * `<element>[] [ ... ]` — `string[]`, `s32[]`, `object[]`, `null[]`, the heterogeneous `any[]`,
 * and the nested `array[]`. A bare `[]` is the empty array. Comments are `//` line and block.
 */

module.exports = grammar({
  name: 'nya',

  extras: $ => [/\s/, $.comment],

  word: $ => $.identifier,

  // `null` alone is the null value; `null[] [ ... ]` is an array of nulls. Distinguishing them needs
  // two tokens of lookahead past `null`, so tree-sitter is told the split is expected, not a mistake.
  conflicts: $ => [
    [$.array, $.null],
  ],

  rules: {
    document: $ => seq($.header, field('root', $.object)),

    // `nya <version> <checksum>`. Both numbers are integers; the checksum is a u64 and may be large.
    header: $ => seq(
      'nya',
      field('version', $.number),
      field('checksum', $.number),
    ),

    // `{ ... }`, with the `object` keyword optional in front of the brace.
    object: $ => seq(
      optional(field('type', alias('object', $.type))),
      '{',
      repeat($.member),
      '}',
    ),

    // `key: value;` — the `;` is optional, so a hand written file may leave the last one off.
    member: $ => seq(
      field('key', $.identifier),
      ':',
      field('value', $._value),
      optional(';'),
    ),

    _value: $ => choice(
      $.object,
      $.array,
      $.scalar,
      $.string,
      $.number,
      $.boolean,
      $.null,
    ),

    // A typed scalar: `s32 5`, `string "x"`, `f32 0x1.5p+3`, `b8 true`, `char "c"`.
    scalar: $ => seq(
      field('type', $.type),
      field('value', choice($.string, $.number, $.boolean)),
    ),

    // `[]` on its own is the empty array. Otherwise an array names its element type, then `[]`,
    // then its bracketed elements: `string[] ["a", "b"]`, `s32[] [1, 2]`, `any[] [s32 1, b8 true]`.
    array: $ => choice(
      seq('[', ']'),
      seq(
        field('type', choice(
          $.type,
          alias('object', $.type),
          alias('any', $.type),
          alias('null', $.type),
        )),
        '[', ']',
        seq(
          '[',
          repeat(seq($._value, optional(','))),
          ']',
        ),
      ),
    ),

    boolean: _ => choice('true', 'false'),

    null: _ => 'null',

    string: $ => seq(
      '"',
      repeat(choice(
        $.escape_sequence,
        token.immediate(prec(1, /[^"\\]+/)),
      )),
      '"',
    ),

    // The lexer honours `\"`, `\\`, `\n`, `\t`, `\r`, `\0`, and keeps any other backslash pair as is.
    escape_sequence: _ => token.immediate(/\\./),

    // Decimal (with optional fraction and `e`/`E` exponent), hexadecimal (with optional fraction
    // and `p`/`P` binary exponent, so a float survives a bit-exact round trip), or binary. A leading
    // minus is lexed on its own by the engine and stitched back on, so it is part of the number here.
    number: _ => token(seq(
      optional('-'),
      choice(
        /0[xX][0-9a-fA-F]+(\.[0-9a-fA-F]+)?([pP][+-]?[0-9]+)?/,
        /0[bB][01]+/,
        /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/,
      ),
    )),

    type: _ => choice(
      'void',
      'b8', 'b16', 'b32', 'b64', 'b128',
      'u8', 'u16', 'u32', 'u64', 'u128',
      's8', 's16', 's32', 's64', 's128',
      'f16', 'f32', 'f64', 'f128',
      'char', 'wchar', 'string', 'wstring',
      'array',
    ),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    comment: _ => token(choice(
      seq('//', /[^\n]*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),
  },
});
