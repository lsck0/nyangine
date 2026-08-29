#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_SERDE_NYA_INDENT_SIZE      4
#define _NYA_SERDE_NYA_OBFUSCATED_MAGIC 0xA7

/** Bounds recursion so a hostile or absurdly nested document fails loudly instead of blowing the stack. */
#define _NYA_SERDE_NYA_DEPTH_MAX 128

/** "nyangine". Obfuscation only, see the note on NYA_SERDE_OBFUSCATE. */
NYA_INTERNAL const u8 _NYA_SERDE_NYA_XOR_KEY[] = {
    0x6E, 0x79, 0x61, 0x6E, 0x67, 0x69, 0x6E, 0x65,
};

typedef struct _NYA_SerdeNyaParser _NYA_SerdeNyaParser;

/**
 * Parse state.
 *
 * Bundled rather than passed as separate arguments because every recursive step needs all of it,
 * and because `depth` has to be shared: a per call local would not bound anything.
 * */
struct _NYA_SerdeNyaParser {
    NYA_Arena* arena;
    NYA_Lexer* lexer;
    u64        index;
    u32        depth;
};

NYA_INTERNAL void _nya_serde_nya_write_object(NYA_String* out, const NYA_Object* object, u32 indent, NYA_SerdeFlags flags);
NYA_INTERNAL void _nya_serde_nya_write_value(NYA_String* out, const NYA_Value* value, u32 indent, NYA_SerdeFlags flags);
NYA_INTERNAL void _nya_serde_nya_write_string(NYA_String* out, NYA_ConstCString text);
NYA_INTERNAL void _nya_serde_nya_write_indent(NYA_String* out, u32 indent);

NYA_INTERNAL NYA_Error _nya_serde_nya_parse_members(_NYA_SerdeNyaParser* parser, NYA_Object* object);
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_typed_value(_NYA_SerdeNyaParser* parser, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_array(_NYA_SerdeNyaParser* parser, NYA_Type element_type, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_value(_NYA_SerdeNyaParser* parser, NYA_Type type, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_number(_NYA_SerdeNyaParser* parser, NYA_Type type, OUT NYA_Value* out_value);

NYA_INTERNAL void       _nya_serde_nya_skip_trivia(_NYA_SerdeNyaParser* parser);
NYA_INTERNAL NYA_Token* _nya_serde_nya_peek(_NYA_SerdeNyaParser* parser);
NYA_INTERNAL b8         _nya_serde_nya_at_symbol(_NYA_SerdeNyaParser* parser, char symbol);
NYA_INTERNAL b8         _nya_serde_nya_accept_symbol(_NYA_SerdeNyaParser* parser, char symbol);
NYA_INTERNAL b8         _nya_serde_nya_accept_array_marker(_NYA_SerdeNyaParser* parser);
NYA_INTERNAL b8         _nya_serde_nya_token_equals(_NYA_SerdeNyaParser* parser, const NYA_Token* token, NYA_ConstCString text);

NYA_INTERNAL b8   _nya_serde_nya_value_is_true(const NYA_Value* value);
NYA_INTERNAL void _nya_serde_nya_set_boolean(NYA_Value* value, b8 truth);

NYA_INTERNAL void _nya_serde_nya_xor(u8* data, u64 length);
NYA_INTERNAL u64  _nya_serde_nya_checksum_value(const NYA_Value* value);
NYA_INTERNAL u64  _nya_serde_nya_mix(u64 a, u64 b);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_String* nya_serde_nya_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) {
    nya_assert(arena != nullptr);
    nya_assert(object != nullptr);

    b8 obfuscate = nya_flag_check(flags, NYA_SERDE_OBFUSCATE);
    b8 pretty    = nya_flag_check(flags, NYA_SERDE_PRETTY) && !obfuscate;

    NYA_String* result = nya_string_create_with_capacity(arena, 512);

    nya_string_extend_sprintf(result, NYA_SERDE_NYA_MAGIC " %d " FMTu64, NYA_SERDE_NYA_VERSION, nya_serde_nya_checksum(object));
    nya_string_extend(result, pretty ? "\n" : " ");

    _nya_serde_nya_write_object(result, object, 0, pretty ? NYA_SERDE_PRETTY : NYA_SERDE_NONE);

    if (obfuscate) {
        NYA_String* encoded = nya_string_create(arena);
        nya_base64_encode(encoded, result->items, result->length);
        _nya_serde_nya_xor(encoded->items, encoded->length);

        nya_string_clear(result);
        nya_string_push_back(result, (u8)_NYA_SERDE_NYA_OBFUSCATED_MAGIC);
        nya_string_extend(result, encoded);
    }

    return result;
}

NYA_Error nya_serde_nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    *out_object = nullptr;
    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_PARSE, "empty input");

    b8 skip_checksum = nya_flag_check(flags, NYA_SERDE_NO_CHECKSUM);

    // Recover the plain text first, so the parser below never has to know how it arrived.
    NYA_CString text = nullptr;

    if (data[0] == _NYA_SERDE_NYA_OBFUSCATED_MAGIC) {
        u64 payload_length = size - 1;

        u8* scratch = nya_arena_alloc(arena, payload_length);
        nya_memcpy(scratch, data + 1, payload_length);
        _nya_serde_nya_xor(scratch, payload_length);

        NYA_String* decoded = nya_string_create(arena);
        nya_base64_decode(decoded, scratch, payload_length);

        text = nya_string_to_cstring(arena, decoded);
    } else {
        text = nya_string_to_cstring(arena, &(NYA_String){ .length = size, .items = (u8*)data });
    }

    NYA_Lexer lexer = nya_lexer_create(text);
    nya_lexer_run(&lexer);

    // Destroyed on every path out, of which there are ten below, all of them early returns on a
    // malformed document. Nothing in the parsed object points into the lexer: keys and string
    // values are allocated from the caller's arena, not from the token stream.
    defer nya_lexer_destroy(&lexer);

    _NYA_SerdeNyaParser parser = { .arena = arena, .lexer = &lexer, .index = 0, .depth = 0 };

    /* header: magic, version, checksum */

    _nya_serde_nya_skip_trivia(&parser);

    NYA_Token* magic = _nya_serde_nya_peek(&parser);
    if (magic == nullptr || magic->type != NYA_TOKEN_IDENT || !_nya_serde_nya_token_equals(&parser, magic, NYA_SERDE_NYA_MAGIC)) {
        return nya_error(NYA_ERROR_PARSE, "not a nya document: expected it to start with '" NYA_SERDE_NYA_MAGIC "'");
    }
    parser.index++;

    _nya_serde_nya_skip_trivia(&parser);

    NYA_Token* version_token = _nya_serde_nya_peek(&parser);
    if (version_token == nullptr || version_token->type != NYA_TOKEN_NUMBER_INTEGER) {
        return nya_error(NYA_ERROR_PARSE, "missing or non-integer version in the header");
    }

    s32 version = 0;
    if (!nya_type_parse(NYA_TYPE_S32, (const u8*)lexer.source + version_token->source_location, version_token->length, &version)) {
        return nya_error(NYA_ERROR_PARSE, "the version is not an s32");
    }
    if (version != NYA_SERDE_NYA_VERSION) {
        return nya_error(NYA_ERROR_PARSE, "unsupported format version " FMTs32 ", this build reads %d", version, NYA_SERDE_NYA_VERSION);
    }
    parser.index++;

    _nya_serde_nya_skip_trivia(&parser);

    NYA_Token* checksum_token = _nya_serde_nya_peek(&parser);
    if (checksum_token == nullptr || checksum_token->type != NYA_TOKEN_NUMBER_INTEGER) {
        return nya_error(NYA_ERROR_PARSE, "missing or non-integer checksum in the header");
    }

    u64 expected_checksum = 0;
    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)lexer.source + checksum_token->source_location, checksum_token->length, &expected_checksum)) {
        return nya_error(NYA_ERROR_PARSE, "the checksum is not a u64");
    }
    parser.index++;

    /* body */

    if (!_nya_serde_nya_accept_symbol(&parser, '{')) return nya_error(NYA_ERROR_PARSE, "expected '{' to open the root object");

    NYA_Object* object = nya_object_create(arena);
    NYA_TRY(_nya_serde_nya_parse_members(&parser, object));

    if (!_nya_serde_nya_accept_symbol(&parser, '}')) return nya_error(NYA_ERROR_PARSE, "expected '}' to close the root object");

    if (!skip_checksum) {
        u64 actual_checksum = nya_serde_nya_checksum(object);
        if (actual_checksum != expected_checksum) {
            return nya_error(
                NYA_ERROR_CORRUPT,
                "checksum mismatch: the header claims " FMTu64 " but the contents hash to " FMTu64,
                expected_checksum,
                actual_checksum
            );
        }
    }

    *out_object = object;
    return NYA_OK;
}

/*
 * The accumulation below is deliberate wraparound, as in every other hash here, so it is
 * exempted the same way _nya_serde_nya_mix already is. Without this a sanitized build aborts
 * the moment it checksums an object, since FLAGS_SANITIZE pairs unsigned-integer-overflow with
 * -fno-sanitize-recover=all.
 * */
__attr_no_sanitize("unsigned-integer-overflow") u64 nya_serde_nya_checksum(const NYA_Object* object) {
    nya_assert(object != nullptr);

    u64 checksum = 0;

    // Summed, not XORed. A dict has no order to preserve so the combine must be commutative, but
    // XOR is commutative *and* self cancelling: under XOR, swapping the values of two keys leaves
    // the total unchanged, and so does any pair of entries that happen to hash alike. Adding a
    // mixed key/value pair keeps the order independence without either weakness.
    nya_dict_foreach_key (object, key) {
        const NYA_Value* value = nya_object_get(object, *key);

        u64 key_hash   = nya_crc64((const u8*)*key, strlen(*key));
        u64 value_hash = _nya_serde_nya_checksum_value(value);

        checksum += _nya_serde_nya_mix(key_hash, value_hash);
    }

    // Fold in the entry count, so adding an entry that happens to mix to zero is still visible.
    return _nya_serde_nya_mix(checksum, object->length);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_serde_nya_write_object(NYA_String* out, const NYA_Object* object, u32 indent, NYA_SerdeFlags flags) {
    b8 pretty = nya_flag_check(flags, NYA_SERDE_PRETTY);

    if (object->length == 0) {
        nya_string_extend(out, "{}");
        return;
    }

    nya_string_extend(out, pretty ? "{\n" : "{");

    b8 first = true;
    nya_dict_foreach_key (object, key) {
        const NYA_Value* value = nya_object_get(object, *key);

        if (!first) nya_string_extend(out, pretty ? "\n" : " ");
        first = false;

        if (pretty) _nya_serde_nya_write_indent(out, indent + 1);

        nya_string_extend(out, *key);
        nya_string_extend(out, pretty ? ": " : ":");

        // Every key names its own type, so clear whatever the array writer set for its elements.
        NYA_SerdeFlags member_flags = flags;
        nya_flag_unset(member_flags, _NYA_SERDE_NO_TYPE_SPECIFIER);

        _nya_serde_nya_write_value(out, value, indent + 1, member_flags);
        nya_string_extend(out, ";");
    }

    if (pretty) {
        nya_string_extend(out, "\n");
        _nya_serde_nya_write_indent(out, indent);
    }
    nya_string_extend(out, "}");
}

NYA_INTERNAL void _nya_serde_nya_write_value(NYA_String* out, const NYA_Value* value, u32 indent, NYA_SerdeFlags flags) {
    b8 pretty        = nya_flag_check(flags, NYA_SERDE_PRETTY);
    b8 name_the_type = !nya_flag_check(flags, _NYA_SERDE_NO_TYPE_SPECIFIER);

    // null names itself, and an array writes its type as `element[]` in its own branch below.
    if (value->type == NYA_TYPE_NULL || value->type == NYA_TYPE_ARRAY) name_the_type = false;

    if (name_the_type) {
        nya_string_extend(out, NYA_TYPE_NAME_MAP[value->type]);
        nya_string_extend(out, " ");
    }

    switch (value->type) {
        case NYA_TYPE_NULL:   nya_string_extend(out, "null"); break;

        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128:   nya_string_extend(out, _nya_serde_nya_value_is_true(value) ? "true" : "false"); break;

        case NYA_TYPE_U8:     nya_string_extend_sprintf(out, FMTu8, value->as_u8); break;
        case NYA_TYPE_U16:    nya_string_extend_sprintf(out, FMTu16, value->as_u16); break;
        case NYA_TYPE_U32:    nya_string_extend_sprintf(out, FMTu32, value->as_u32); break;
        case NYA_TYPE_U64:    nya_string_extend_sprintf(out, FMTu64, value->as_u64); break;
        case NYA_TYPE_U128:   nya_string_extend(out, nya_u128_to_string(out->arena, value->as_u128)); break;

        case NYA_TYPE_S8:     nya_string_extend_sprintf(out, FMTs8, value->as_s8); break;
        case NYA_TYPE_S16:    nya_string_extend_sprintf(out, FMTs16, value->as_s16); break;
        case NYA_TYPE_S32:    nya_string_extend_sprintf(out, FMTs32, value->as_s32); break;
        case NYA_TYPE_S64:    nya_string_extend_sprintf(out, FMTs64, value->as_s64); break;
        case NYA_TYPE_S128:   nya_string_extend(out, nya_s128_to_string(out->arena, value->as_s128)); break;

        /*
         * Hexadecimal, so a float survives the round trip bit for bit.
         *
         * Decimal only round trips if both the writing and the reading are correctly rounded, and
         * enough digits is not on its own enough: the reader accumulates digits and divides, and the
         * error from that division has nowhere to go for an f128. Measured before this change, 92 of
         * 300 random f128 values came back altered — and because the format checksums its values
         * rather than its text, an altered value fails the checksum and the document is rejected as
         * corrupt rather than merely being slightly wrong.
         *
         * A hex float has no such gap. Four bits per digit, an exponent that scales by two, so both
         * directions are exact by construction. It is also what C itself writes with %a, so the text
         * is readable by strtod and by a person who knows the format.
         *
         * JSON keeps its decimal form. Hex floats are not valid JSON, and that format's whole reason
         * for existing is being readable by things that are not this engine.
         * */
        case NYA_TYPE_F16:    nya_string_extend_sprintf(out, "%a", (f64)value->as_f16); break;
        case NYA_TYPE_F32:    nya_string_extend_sprintf(out, "%a", (f64)value->as_f32); break;
        case NYA_TYPE_F64:    nya_string_extend_sprintf(out, "%a", value->as_f64); break;
        case NYA_TYPE_F128:   nya_string_extend_sprintf(out, "%La", value->as_f128); break;

        case NYA_TYPE_CHAR:   _nya_serde_nya_write_string(out, (char[2]){ value->as_char, '\0' }); break;
        case NYA_TYPE_STRING: _nya_serde_nya_write_string(out, value->as_string ? value->as_string : ""); break;

        case NYA_TYPE_OBJECT: _nya_serde_nya_write_object(out, &value->as_object, indent, flags); break;

        case NYA_TYPE_ARRAY:  {
            if (value->as_array.length == 0) {
                nya_string_extend(out, "[]");
                break;
            }

            // One shared element type keeps the type name out of every element. A mixed array
            // cannot do that, so it declares `any` and every element names itself.
            NYA_Type element_type = value->as_array.items[0].type;
            b8       homogeneous  = true;
            for (u64 i = 1; i < value->as_array.length; i++) {
                if (value->as_array.items[i].type != element_type) {
                    homogeneous = false;
                    break;
                }
            }

            nya_string_extend(out, homogeneous ? NYA_TYPE_NAME_MAP[element_type] : NYA_SERDE_NYA_ANY_TYPE);
            nya_string_extend(out, pretty ? "[] [\n" : "[] [");

            NYA_SerdeFlags element_flags = flags;
            if (homogeneous)
                nya_flag_set(element_flags, _NYA_SERDE_NO_TYPE_SPECIFIER);
            else
                nya_flag_unset(element_flags, _NYA_SERDE_NO_TYPE_SPECIFIER);

            b8 first = true;
            nya_array_foreach (&value->as_array, element) {
                if (!first) nya_string_extend(out, pretty ? ",\n" : ", ");
                first = false;

                if (pretty) _nya_serde_nya_write_indent(out, indent + 1);
                _nya_serde_nya_write_value(out, element, indent + 1, element_flags);
            }

            if (pretty) {
                nya_string_extend(out, "\n");
                _nya_serde_nya_write_indent(out, indent);
            }
            nya_string_extend(out, "]");
        } break;

        default: nya_log_panic("Cannot serialize a value of type %s.", NYA_TYPE_NAME_MAP[value->type]);
    }
}

NYA_INTERNAL void _nya_serde_nya_write_string(NYA_String* out, NYA_ConstCString text) {
    nya_string_extend(out, "\"");

    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            case '"':  nya_string_extend(out, "\\\""); break;
            case '\\': nya_string_extend(out, "\\\\"); break;
            case '\n': nya_string_extend(out, "\\n"); break;
            case '\t': nya_string_extend(out, "\\t"); break;
            case '\r': nya_string_extend(out, "\\r"); break;
            default:   nya_string_push_back(out, (u8)*cursor); break;
        }
    }

    nya_string_extend(out, "\"");
}

NYA_INTERNAL void _nya_serde_nya_write_indent(NYA_String* out, u32 indent) {
    for (u32 i = 0; i < indent * _NYA_SERDE_NYA_INDENT_SIZE; i++) nya_string_push_back(out, ' ');
}

/*
 * ─────────────────────────────────────────────────────────
 * PARSING
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Error _nya_serde_nya_parse_members(_NYA_SerdeNyaParser* parser, NYA_Object* object) {
    if (parser->depth >= _NYA_SERDE_NYA_DEPTH_MAX) return nya_error(NYA_ERROR_PARSE, "object nesting is too deep");
    parser->depth++;

    while (true) {
        _nya_serde_nya_skip_trivia(parser);

        NYA_Token* token = _nya_serde_nya_peek(parser);
        if (token == nullptr) break;
        if (token->type == NYA_TOKEN_SYMBOL && token->symbol == '}') break;

        if (token->type != NYA_TOKEN_IDENT) {
            return nya_error(NYA_ERROR_PARSE, "expected a key, got '%.*s'", (int)token->length, parser->lexer->source + token->source_location);
        }

        NYA_CString key = nya_arena_alloc(parser->arena, token->length + 1);
        nya_memcpy(key, parser->lexer->source + token->source_location, token->length);
        key[token->length] = '\0';
        parser->index++;

        if (!_nya_serde_nya_accept_symbol(parser, ':')) return nya_error(NYA_ERROR_PARSE, "expected ':' after the key '%s'", key);

        NYA_Value value;
        NYA_TRY(_nya_serde_nya_parse_typed_value(parser, &value));
        nya_object_set(object, key, value);

        // Optional, so a hand written file may leave the last one off.
        _nya_serde_nya_accept_symbol(parser, ';');
    }

    parser->depth--;
    return NYA_OK;
}

/** Reads `<type> <value>`, `<type>[] [...]`, a bare `[]`, or `null` — everything that can follow a colon. */
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_typed_value(_NYA_SerdeNyaParser* parser, OUT NYA_Value* out_value) {
    _nya_serde_nya_skip_trivia(parser);

    NYA_Token* token = _nya_serde_nya_peek(parser);
    if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unexpected end of input where a value was expected");

    // An empty array carries no element type, so it is spelled `[]` and nothing else.
    if (_nya_serde_nya_accept_array_marker(parser)) {
        *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = nya_array_create_on_stack(parser->arena, NYA_Value) };
        return NYA_OK;
    }

    // A brace with no `object` in front of it is still an object.
    if (token->type == NYA_TOKEN_SYMBOL && token->symbol == '{') return _nya_serde_nya_parse_value(parser, NYA_TYPE_OBJECT, out_value);

    if (token->type != NYA_TOKEN_IDENT) {
        return nya_error(NYA_ERROR_PARSE, "expected a type name, got '%.*s'", (int)token->length, parser->lexer->source + token->source_location);
    }

    if (_nya_serde_nya_token_equals(parser, token, "null")) {
        parser->index++;
        *out_value = (NYA_Value){ .type = NYA_TYPE_NULL };
        return NYA_OK;
    }

    // `any[]` heads a heterogeneous array. It is not a NYA_Type, so it is matched by name before
    // the type table is consulted.
    if (_nya_serde_nya_token_equals(parser, token, NYA_SERDE_NYA_ANY_TYPE)) {
        parser->index++;
        if (!_nya_serde_nya_accept_array_marker(parser)) return nya_error(NYA_ERROR_PARSE, "'any' is only valid as the 'any[]' array header");

        return _nya_serde_nya_parse_array(parser, NYA_TYPE_COUNT, out_value);
    }

    NYA_Type         type      = NYA_TYPE_NULL;
    NYA_ConstCString type_name = nullptr;
    if (!nya_type_name_parse((const u8*)parser->lexer->source + token->source_location, token->length, &type, &type_name)) {
        return nya_error(NYA_ERROR_PARSE, "unknown type '%.*s'", (int)token->length, parser->lexer->source + token->source_location);
    }
    parser->index++;

    if (_nya_serde_nya_accept_array_marker(parser)) return _nya_serde_nya_parse_array(parser, type, out_value);

    return _nya_serde_nya_parse_value(parser, type, out_value);
}

/** Reads `[a, b, c]`. `element_type` is NYA_TYPE_COUNT for an `any[]`, whose elements describe themselves. */
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_array(_NYA_SerdeNyaParser* parser, NYA_Type element_type, OUT NYA_Value* out_value) {
    if (parser->depth >= _NYA_SERDE_NYA_DEPTH_MAX) return nya_error(NYA_ERROR_PARSE, "array nesting is too deep");
    parser->depth++;

    if (!_nya_serde_nya_accept_symbol(parser, '[')) return nya_error(NYA_ERROR_PARSE, "expected '[' to open an array");

    NYA_ArrayᐸNYA_Valueᐳ elements = nya_array_create_on_stack(parser->arena, NYA_Value);

    while (true) {
        _nya_serde_nya_skip_trivia(parser);

        NYA_Token* token = _nya_serde_nya_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated array");

        if (token->type == NYA_TOKEN_SYMBOL && token->symbol == ']') {
            parser->index++;
            break;
        }

        NYA_Value element;

        // An `any[]` element, and a nested array element, both carry their own type name, so they
        // go back through the full path. Everything else inherits the header's type.
        if (element_type == NYA_TYPE_COUNT || element_type == NYA_TYPE_ARRAY) {
            NYA_TRY(_nya_serde_nya_parse_typed_value(parser, &element));
        } else {
            NYA_TRY(_nya_serde_nya_parse_value(parser, element_type, &element));
        }

        nya_array_push_back(&elements, element);

        // Optional, so a trailing comma is accepted.
        _nya_serde_nya_accept_symbol(parser, ',');
    }

    parser->depth--;

    *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = elements };
    return NYA_OK;
}

/** Reads a bare value whose type the caller has already established. */
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_value(_NYA_SerdeNyaParser* parser, NYA_Type type, OUT NYA_Value* out_value) {
    _nya_serde_nya_skip_trivia(parser);

    NYA_Token* token = _nya_serde_nya_peek(parser);
    if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unexpected end of input while reading a %s", NYA_TYPE_NAME_MAP[type]);

    const char* source = parser->lexer->source + token->source_location;

    switch (type) {
        case NYA_TYPE_NULL: {
            parser->index++;
            *out_value = (NYA_Value){ .type = NYA_TYPE_NULL };
            return NYA_OK;
        }

        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: {
            b8 truth = false;

            if (token->type == NYA_TOKEN_IDENT && _nya_serde_nya_token_equals(parser, token, "true"))
                truth = true;
            else if (token->type != NYA_TOKEN_IDENT || !_nya_serde_nya_token_equals(parser, token, "false")) {
                return nya_error(NYA_ERROR_PARSE, "expected true or false, got '%.*s'", (int)token->length, source);
            }
            parser->index++;

            NYA_Value value = { .type = type };
            _nya_serde_nya_set_boolean(&value, truth);

            *out_value = value;
            return NYA_OK;
        }

        case NYA_TYPE_STRING: {
            if (token->type != NYA_TOKEN_STRING) {
                return nya_error(NYA_ERROR_PARSE, "expected a quoted string, got '%.*s'", (int)token->length, source);
            }

            // Unescaping only ever shortens, so one allocation of the raw length is always enough.
            NYA_CString unescaped = nya_arena_alloc(parser->arena, token->length + 1);
            u64         written   = 0;

            for (u64 i = 0; i < token->length; i++) {
                if (source[i] != '\\' || i + 1 >= token->length) {
                    unescaped[written++] = source[i];
                    continue;
                }

                i++;
                switch (source[i]) {
                    case '"':  unescaped[written++] = '"'; break;
                    case '\\': unescaped[written++] = '\\'; break;
                    case 'n':  unescaped[written++] = '\n'; break;
                    case 't':  unescaped[written++] = '\t'; break;
                    case 'r':  unescaped[written++] = '\r'; break;
                    case '0':  unescaped[written++] = '\0'; break;

                    // Unknown escape: keep both characters rather than silently dropping data.
                    default:
                        unescaped[written++] = '\\';
                        unescaped[written++] = source[i];
                        break;
                }
            }

            unescaped[written] = '\0';
            parser->index++;

            *out_value = (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = unescaped };
            return NYA_OK;
        }

        case NYA_TYPE_CHAR: {
            if (token->type != NYA_TOKEN_STRING || token->length == 0) {
                return nya_error(NYA_ERROR_PARSE, "expected a one character string for a char, got '%.*s'", (int)token->length, source);
            }
            parser->index++;

            *out_value = (NYA_Value){ .type = NYA_TYPE_CHAR, .as_char = source[0] };
            return NYA_OK;
        }

        case NYA_TYPE_OBJECT: {
            // `object` in front of the brace is optional; the brace alone is unambiguous.
            if (token->type == NYA_TOKEN_IDENT && _nya_serde_nya_token_equals(parser, token, "object")) parser->index++;

            if (!_nya_serde_nya_accept_symbol(parser, '{')) return nya_error(NYA_ERROR_PARSE, "expected '{' to open an object");

            NYA_Object* nested = nya_object_create(parser->arena);
            NYA_TRY(_nya_serde_nya_parse_members(parser, nested));

            if (!_nya_serde_nya_accept_symbol(parser, '}')) return nya_error(NYA_ERROR_PARSE, "expected '}' to close an object");

            *out_value = (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested };
            return NYA_OK;
        }

        case NYA_TYPE_ARRAY: return _nya_serde_nya_parse_typed_value(parser, out_value);

        default:             return _nya_serde_nya_parse_number(parser, type, out_value);
    }
}

/**
 * Reads a numeric literal, stitching a leading minus back on.
 *
 * The lexer emits '-' as its own symbol, deliberately: folding it into the number would turn
 * `a - b` into `a` and `-b` for every other user of the lexer. So the sign is reattached here,
 * where a minus in front of a number is unambiguous.
 * */
NYA_INTERNAL NYA_Error _nya_serde_nya_parse_number(_NYA_SerdeNyaParser* parser, NYA_Type type, OUT NYA_Value* out_value) {
    NYA_Token* token    = _nya_serde_nya_peek(parser);
    b8         negative = token->type == NYA_TOKEN_SYMBOL && token->symbol == '-';

    if (negative) {
        parser->index++;

        token = _nya_serde_nya_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "expected a number after '-'");
    }

    if (token->type != NYA_TOKEN_NUMBER_INTEGER && token->type != NYA_TOKEN_NUMBER_FLOAT) {
        return nya_error(
            NYA_ERROR_PARSE,
            "expected a %s literal, got '%.*s'",
            NYA_TYPE_NAME_MAP[type],
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    // One scratch buffer, so the sign and the digits are contiguous for nya_type_parse. The longest
    // thing that reaches here is an f128 in decimal, which stays well inside this.
    char text[192];
    u64  length = 0;

    if (negative) text[length++] = '-';

    /*
     * Refused rather than clamped, matching _nya_serde_json_parse_number.
     *
     * The digits were truncated to whatever fit, silently, so a literal longer than this buffer was
     * parsed from its prefix and came back orders of magnitude from what the document said. That is
     * worse here than it was in JSON: this is the save format, so a number that does not survive a
     * round trip is a corrupted save rather than a misread response — and the checksum is computed
     * over the object that was parsed, so it agrees with the wrong value.
     *
     * The bound is generous by a wide margin: an f128 in decimal is nowhere near it.
     */
    u64 digits_length = token->length;
    if (digits_length > sizeof(text) - length - 1) {
        return nya_error(
            NYA_ERROR_PARSE,
            "the number at line " FMTu32 " has " FMTu64 " digits, past the %zu this parser accepts",
            token->line_number,
            digits_length,
            sizeof(text) - length - 1
        );
    }

    nya_memcpy(text + length, parser->lexer->source + token->source_location, digits_length);
    length       += digits_length;
    text[length]  = '\0';

    // as_u128 is the widest member, so writing through it covers every numeric type in the union.
    NYA_Value value = { .type = type };
    if (!nya_type_parse(type, (const u8*)text, length, &value.as_u128)) {
        return nya_error(NYA_ERROR_PARSE, "'%s' is not a valid %s", text, NYA_TYPE_NAME_MAP[type]);
    }
    parser->index++;

    *out_value = value;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * TOKEN HELPERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Skips comments.
 *
 * One token type now, since the lexer recognises a comment itself. This used to reassemble one from
 * the two adjacent symbol tokens it arrived as, checking source offsets so that a slash and a star
 * with a space between them were not mistaken for an opener — all of which the lexer does better,
 * because it is looking at characters rather than at tokens that have already lost their spacing.
 * */
NYA_INTERNAL void _nya_serde_nya_skip_trivia(_NYA_SerdeNyaParser* parser) {
    while (parser->index < parser->lexer->tokens->length &&
           parser->lexer->tokens->items[parser->index].type == NYA_TOKEN_COMMENT) {
        parser->index++;
    }
}

/** Returns nullptr at end of input, so EOF and "ran off the end" are one case for every caller. */
NYA_INTERNAL NYA_Token* _nya_serde_nya_peek(_NYA_SerdeNyaParser* parser) {
    if (parser->index >= parser->lexer->tokens->length) return nullptr;

    NYA_Token* token = &parser->lexer->tokens->items[parser->index];
    return token->type == NYA_TOKEN_EOF ? nullptr : token;
}

NYA_INTERNAL b8 _nya_serde_nya_at_symbol(_NYA_SerdeNyaParser* parser, char symbol) {
    NYA_Token* token = _nya_serde_nya_peek(parser);
    return token != nullptr && token->type == NYA_TOKEN_SYMBOL && token->symbol == (u8)symbol;
}

NYA_INTERNAL b8 _nya_serde_nya_accept_symbol(_NYA_SerdeNyaParser* parser, char symbol) {
    _nya_serde_nya_skip_trivia(parser);

    if (!_nya_serde_nya_at_symbol(parser, symbol)) return false;

    parser->index++;
    return true;
}

/** Consumes the `[]` that marks an array type. Both brackets or neither. */
NYA_INTERNAL b8 _nya_serde_nya_accept_array_marker(_NYA_SerdeNyaParser* parser) {
    _nya_serde_nya_skip_trivia(parser);

    if (parser->index + 1 >= parser->lexer->tokens->length) return false;

    NYA_Token* open  = &parser->lexer->tokens->items[parser->index];
    NYA_Token* close = &parser->lexer->tokens->items[parser->index + 1];

    if (open->type != NYA_TOKEN_SYMBOL || open->symbol != '[') return false;
    if (close->type != NYA_TOKEN_SYMBOL || close->symbol != ']') return false;

    parser->index += 2;
    return true;
}

NYA_INTERNAL b8 _nya_serde_nya_token_equals(_NYA_SerdeNyaParser* parser, const NYA_Token* token, NYA_ConstCString text) {
    u64 length = strlen(text);
    if (token->length != length) return false;

    return strncmp(parser->lexer->source + token->source_location, text, length) == 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * VALUE HELPERS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_serde_nya_value_is_true(const NYA_Value* value) {
    switch (value->type) {
        case NYA_TYPE_B8:   return value->as_b8 != 0;
        case NYA_TYPE_B16:  return value->as_b16 != 0;
        case NYA_TYPE_B32:  return value->as_b32 != 0;
        case NYA_TYPE_B64:  return value->as_b64 != 0;
        case NYA_TYPE_B128: return value->as_b128 != 0;
        default:            nya_unreachable();
    }
}

NYA_INTERNAL void _nya_serde_nya_set_boolean(NYA_Value* value, b8 truth) {
    switch (value->type) {
        case NYA_TYPE_B8:   value->as_b8 = truth; break;
        case NYA_TYPE_B16:  value->as_b16 = truth; break;
        case NYA_TYPE_B32:  value->as_b32 = truth; break;
        case NYA_TYPE_B64:  value->as_b64 = truth; break;
        case NYA_TYPE_B128: value->as_b128 = truth; break;
        default:            nya_unreachable();
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * CHECKSUM AND OBFUSCATION
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_serde_nya_xor(u8* data, u64 length) {
    for (u64 i = 0; i < length; i++) data[i] ^= _NYA_SERDE_NYA_XOR_KEY[i % sizeof(_NYA_SERDE_NYA_XOR_KEY)];
}

/** splitmix64's finalizer. Cheap, and it makes every input bit affect every output bit. */
__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u64 _nya_serde_nya_mix(u64 a, u64 b) {
    u64 mixed  = a + 0x9E3779B97F4A7C15ULL + (b << 6) + (b >> 2);
    mixed     ^= mixed >> 30;
    mixed     *= 0xBF58476D1CE4E5B9ULL;
    mixed     ^= mixed >> 27;
    mixed     *= 0x94D049BB133111EBULL;
    mixed     ^= mixed >> 31;

    return mixed;
}

NYA_INTERNAL u64 _nya_serde_nya_checksum_value(const NYA_Value* value) {
    u32 type = (u32)value->type;
    u64 hash = nya_crc64((const u8*)&type, sizeof(type));

    switch (value->type) {
        case NYA_TYPE_NULL:   break;

        // Hashed as the one bit they mean rather than as raw storage: a b32 holding 1 and one
        // holding 2 are both true, and a checksum that disagreed with that would be surprising.
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128:   hash = _nya_serde_nya_mix(hash, _nya_serde_nya_value_is_true(value) ? 1 : 0); break;

        case NYA_TYPE_U8:     hash = _nya_serde_nya_mix(hash, value->as_u8); break;
        case NYA_TYPE_U16:    hash = _nya_serde_nya_mix(hash, value->as_u16); break;
        case NYA_TYPE_U32:    hash = _nya_serde_nya_mix(hash, value->as_u32); break;
        case NYA_TYPE_U64:    hash = _nya_serde_nya_mix(hash, value->as_u64); break;
        case NYA_TYPE_U128:   hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_u128, sizeof(u128))); break;

        case NYA_TYPE_S8:     hash = _nya_serde_nya_mix(hash, (u64)(s64)value->as_s8); break;
        case NYA_TYPE_S16:    hash = _nya_serde_nya_mix(hash, (u64)(s64)value->as_s16); break;
        case NYA_TYPE_S32:    hash = _nya_serde_nya_mix(hash, (u64)(s64)value->as_s32); break;
        case NYA_TYPE_S64:    hash = _nya_serde_nya_mix(hash, (u64)value->as_s64); break;
        case NYA_TYPE_S128:   hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_s128, sizeof(s128))); break;

        case NYA_TYPE_F16:    hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_f16, sizeof(f16))); break;
        case NYA_TYPE_F32:    hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_f32, sizeof(f32))); break;
        case NYA_TYPE_F64:    hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_f64, sizeof(f64))); break;
        case NYA_TYPE_F128:   hash = _nya_serde_nya_mix(hash, nya_crc64((const u8*)&value->as_f128, sizeof(f128))); break;

        case NYA_TYPE_CHAR:   hash = _nya_serde_nya_mix(hash, (u64)(u8)value->as_char); break;

        case NYA_TYPE_STRING: {
            NYA_ConstCString text = value->as_string ? value->as_string : "";
            hash                  = _nya_serde_nya_mix(hash, nya_crc64((const u8*)text, strlen(text)));
        } break;

        case NYA_TYPE_OBJECT: hash = _nya_serde_nya_mix(hash, nya_serde_nya_checksum(&value->as_object)); break;

        case NYA_TYPE_ARRAY:  {
            // Order matters in an array, unlike in an object, so this folds rather than sums.
            for (u64 i = 0; i < value->as_array.length; i++) {
                hash = _nya_serde_nya_mix(hash, _nya_serde_nya_checksum_value(&value->as_array.items[i]));
            }
            hash = _nya_serde_nya_mix(hash, value->as_array.length);
        } break;

        default: nya_log_panic("Cannot checksum a value of type %s.", NYA_TYPE_NAME_MAP[value->type]);
    }

    return hash;
}
