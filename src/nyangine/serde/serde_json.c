#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_SERDE_JSON_INDENT_SIZE 2

typedef struct _NYA_SerdeJsonParser _NYA_SerdeJsonParser;

struct _NYA_SerdeJsonParser {
    NYA_Arena* arena;
    NYA_Lexer* lexer;
    u64        index;
    u32        depth;
};

NYA_INTERNAL void _nya_serde_json_write_object(NYA_String* out, const NYA_Object* object, u32 indent, b8 pretty);
NYA_INTERNAL void _nya_serde_json_write_value(NYA_String* out, const NYA_Value* value, u32 indent, b8 pretty);
NYA_INTERNAL void _nya_serde_json_write_indent(NYA_String* out, u32 indent);
NYA_INTERNAL void _nya_serde_json_write_real(NYA_String* out, f64 number, NYA_ConstCString format);

NYA_INTERNAL NYA_Error _nya_serde_json_parse_object(_NYA_SerdeJsonParser* parser, OUT NYA_Object** out_object);
NYA_INTERNAL NYA_Error _nya_serde_json_parse_array(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_json_parse_value(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_json_parse_number(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_json_parse_string(_NYA_SerdeJsonParser* parser, OUT NYA_CString* out_text);

NYA_INTERNAL NYA_Token* _nya_serde_json_peek(_NYA_SerdeJsonParser* parser);
NYA_INTERNAL b8         _nya_serde_json_accept_symbol(_NYA_SerdeJsonParser* parser, char symbol);
NYA_INTERNAL b8         _nya_serde_json_token_equals(_NYA_SerdeJsonParser* parser, const NYA_Token* token, NYA_ConstCString text);
NYA_INTERNAL u32        _nya_serde_json_encode_utf8(u32 codepoint, OUT char* out);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_String* nya_serde_json_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) {
    nya_assert(arena != nullptr);
    nya_assert(object != nullptr);

    NYA_String* result = nya_string_create_with_capacity(arena, 512);
    _nya_serde_json_write_object(result, object, 0, nya_flag_check(flags, NYA_SERDE_PRETTY));

    return result;
}

NYA_Error nya_serde_json_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    (void)flags; // JSON carries no checksum and cannot be obfuscated, so none of the flags apply

    *out_object = nullptr;
    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_PARSE, "empty input");

    NYA_CString text  = nya_string_to_cstring(arena, &(NYA_String){ .length = size, .items = (u8*)data });
    NYA_Lexer   lexer = nya_lexer_create(text);
    nya_lexer_run(&lexer);

    _NYA_SerdeJsonParser parser = { .arena = arena, .lexer = &lexer, .index = 0, .depth = 0 };

    NYA_Object* object = nullptr;
    NYA_TRY(_nya_serde_json_parse_object(&parser, &object));

    // Trailing content means the document was two values, or truncated and then resumed. Either
    // way the caller asked for one object and did not get exactly one.
    if (_nya_serde_json_peek(&parser) != nullptr) {
        NYA_Token* token = _nya_serde_json_peek(&parser);
        return nya_error(
            NYA_ERROR_PARSE,
            "trailing content after the root object at line " FMTu32 ": '%.*s'",
            token->line_number,
            (int)token->length,
            lexer.source + token->source_location
        );
    }

    *out_object = object;
    return NYA_OK;
}

void nya_serde_json_escape(NYA_String* out, NYA_ConstCString text) {
    nya_assert(out != nullptr);

    nya_string_extend(out, "\"");

    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        u8 character = (u8)*cursor;

        switch (character) {
            case '"':  nya_string_extend(out, "\\\""); continue;
            case '\\': nya_string_extend(out, "\\\\"); continue;
            case '\n': nya_string_extend(out, "\\n"); continue;
            case '\t': nya_string_extend(out, "\\t"); continue;
            case '\r': nya_string_extend(out, "\\r"); continue;
            case '\b': nya_string_extend(out, "\\b"); continue;
            case '\f': nya_string_extend(out, "\\f"); continue;
            default:   break;
        }

        // JSON forbids raw control characters in strings, and only \u escapes can carry them.
        // Everything at or above 0x20 goes through untouched, which keeps UTF-8 intact.
        if (character < 0x20)
            nya_string_extend_sprintf(out, "\\u%04x", character);
        else
            nya_string_push_back(out, character);
    }

    nya_string_extend(out, "\"");
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

NYA_INTERNAL void _nya_serde_json_write_object(NYA_String* out, const NYA_Object* object, u32 indent, b8 pretty) {
    if (object->length == 0) {
        nya_string_extend(out, "{}");
        return;
    }

    nya_string_extend(out, pretty ? "{\n" : "{");

    b8 first = true;
    nya_dict_foreach_key(object, key) {
        const NYA_Value* value = nya_object_get(object, *key);

        if (!first) nya_string_extend(out, pretty ? ",\n" : ",");
        first = false;

        if (pretty) _nya_serde_json_write_indent(out, indent + 1);

        nya_serde_json_escape(out, *key);
        nya_string_extend(out, pretty ? ": " : ":");

        _nya_serde_json_write_value(out, value, indent + 1, pretty);
    }

    if (pretty) {
        nya_string_extend(out, "\n");
        _nya_serde_json_write_indent(out, indent);
    }
    nya_string_extend(out, "}");
}

NYA_INTERNAL void _nya_serde_json_write_value(NYA_String* out, const NYA_Value* value, u32 indent, b8 pretty) {
    switch (value->type) {
        case NYA_TYPE_NULL:
        case NYA_TYPE_VOID: nya_string_extend(out, "null"); break;

        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: {
            b8 truth = value->type == NYA_TYPE_B8  ? value->as_b8 != 0
                     : value->type == NYA_TYPE_B16 ? value->as_b16 != 0
                     : value->type == NYA_TYPE_B32 ? value->as_b32 != 0
                     : value->type == NYA_TYPE_B64 ? value->as_b64 != 0
                                                   : value->as_b128 != 0;

            nya_string_extend(out, truth ? "true" : "false");
        } break;

        case NYA_TYPE_U8:   nya_string_extend_sprintf(out, FMTu8, value->as_u8); break;
        case NYA_TYPE_U16:  nya_string_extend_sprintf(out, FMTu16, value->as_u16); break;
        case NYA_TYPE_U32:  nya_string_extend_sprintf(out, FMTu32, value->as_u32); break;
        case NYA_TYPE_U64:  nya_string_extend_sprintf(out, FMTu64, value->as_u64); break;

        case NYA_TYPE_S8:   nya_string_extend_sprintf(out, FMTs8, value->as_s8); break;
        case NYA_TYPE_S16:  nya_string_extend_sprintf(out, FMTs16, value->as_s16); break;
        case NYA_TYPE_S32:  nya_string_extend_sprintf(out, FMTs32, value->as_s32); break;
        case NYA_TYPE_S64:  nya_string_extend_sprintf(out, FMTs64, value->as_s64); break;

        // JSON numbers have no width, and 128 bit values do not survive the double that most
        // readers will parse them into. Quoted, they at least arrive intact and are obviously
        // not ordinary numbers.
        case NYA_TYPE_U128: {
            NYA_String* rendered = nya_u128_to_string(out->arena, value->as_u128);
            nya_serde_json_escape(out, nya_string_to_cstring(out->arena, rendered));
        } break;

        case NYA_TYPE_S128: {
            NYA_String* rendered = nya_s128_to_string(out->arena, value->as_s128);
            nya_serde_json_escape(out, nya_string_to_cstring(out->arena, rendered));
        } break;

        case NYA_TYPE_F16:    _nya_serde_json_write_real(out, (f64)value->as_f16, "%.5g"); break;
        case NYA_TYPE_F32:    _nya_serde_json_write_real(out, (f64)value->as_f32, "%.9g"); break;
        case NYA_TYPE_F64:    _nya_serde_json_write_real(out, value->as_f64, "%.17g"); break;
        case NYA_TYPE_F128:   _nya_serde_json_write_real(out, (f64)value->as_f128, "%.17g"); break;

        case NYA_TYPE_CHAR:   nya_serde_json_escape(out, (char[2]){ value->as_char, '\0' }); break;
        case NYA_TYPE_STRING: nya_serde_json_escape(out, value->as_string ? value->as_string : ""); break;

        case NYA_TYPE_OBJECT: _nya_serde_json_write_object(out, &value->as_object, indent, pretty); break;

        case NYA_TYPE_ARRAY:  {
            if (value->as_array.length == 0) {
                nya_string_extend(out, "[]");
                break;
            }

            nya_string_extend(out, pretty ? "[\n" : "[");

            b8 first = true;
            nya_array_foreach (&value->as_array, element) {
                if (!first) nya_string_extend(out, pretty ? ",\n" : ",");
                first = false;

                if (pretty) _nya_serde_json_write_indent(out, indent + 1);
                _nya_serde_json_write_value(out, element, indent + 1, pretty);
            }

            if (pretty) {
                nya_string_extend(out, "\n");
                _nya_serde_json_write_indent(out, indent);
            }
            nya_string_extend(out, "]");
        } break;

        // A pointer's numeric value is meaningless outside the process that produced it, so it is
        // written as null rather than as an integer someone might believe.
        default: nya_string_extend(out, "null"); break;
    }
}

/**
 * Writes a real number as a valid JSON number.
 *
 * JSON has no way to spell NaN or infinity. Emitting them anyway produces a document that strict
 * parsers reject, so they become null, which every parser accepts and no one mistakes for a
 * measurement.
 * */
NYA_INTERNAL void _nya_serde_json_write_real(NYA_String* out, f64 number, NYA_ConstCString format) {
    if (isnan(number) || isinf(number)) {
        nya_string_extend(out, "null");
        return;
    }

    u64 before = out->length;
    nya_string_extend_sprintf(out, format, number);

    // %g drops the fractional part of a whole number, so 1.0 comes out "1". That is a valid JSON
    // number, but it reads back as an integer and the value silently changes type. A trailing
    // ".0" keeps it a real.
    for (u64 i = before; i < out->length; i++) {
        u8 character = out->items[i];
        if (character == '.' || character == 'e' || character == 'E') return;
    }

    nya_string_extend(out, ".0");
}

NYA_INTERNAL void _nya_serde_json_write_indent(NYA_String* out, u32 indent) {
    for (u32 i = 0; i < indent * _NYA_SERDE_JSON_INDENT_SIZE; i++) nya_string_push_back(out, ' ');
}

/*
 * ─────────────────────────────────────────────────────────
 * PARSING
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Error _nya_serde_json_parse_object(_NYA_SerdeJsonParser* parser, OUT NYA_Object** out_object) {
    if (parser->depth >= NYA_SERDE_JSON_DEPTH_MAX) return nya_error(NYA_ERROR_PARSE, "object nesting is too deep");
    parser->depth++;

    if (!_nya_serde_json_accept_symbol(parser, '{')) {
        NYA_Token* token = _nya_serde_json_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "expected '{' but the document ended");

        return nya_error(
            NYA_ERROR_PARSE,
            "expected '{' at line " FMTu32 ", got '%.*s'",
            token->line_number,
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    NYA_Object* object = nya_object_create(parser->arena);

    if (_nya_serde_json_accept_symbol(parser, '}')) {
        parser->depth--;
        *out_object = object;
        return NYA_OK;
    }

    while (true) {
        NYA_CString key = nullptr;
        NYA_TRY(_nya_serde_json_parse_string(parser, &key));

        if (!_nya_serde_json_accept_symbol(parser, ':')) return nya_error(NYA_ERROR_PARSE, "expected ':' after the key '%s'", key);

        NYA_Value value;
        NYA_TRY(_nya_serde_json_parse_value(parser, &value));
        nya_object_set(object, key, value);

        if (_nya_serde_json_accept_symbol(parser, ',')) continue;
        if (_nya_serde_json_accept_symbol(parser, '}')) break;

        NYA_Token* token = _nya_serde_json_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated object");

        return nya_error(
            NYA_ERROR_PARSE,
            "expected ',' or '}' at line " FMTu32 ", got '%.*s'",
            token->line_number,
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    parser->depth--;

    *out_object = object;
    return NYA_OK;
}

NYA_INTERNAL NYA_Error _nya_serde_json_parse_array(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value) {
    if (parser->depth >= NYA_SERDE_JSON_DEPTH_MAX) return nya_error(NYA_ERROR_PARSE, "array nesting is too deep");
    parser->depth++;

    NYA_ArrayᐸNYA_Valueᐳ elements = nya_array_create_on_stack(parser->arena, NYA_Value);

    if (_nya_serde_json_accept_symbol(parser, ']')) {
        parser->depth--;
        *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = elements };
        return NYA_OK;
    }

    while (true) {
        NYA_Value element;
        NYA_TRY(_nya_serde_json_parse_value(parser, &element));
        nya_array_push_back(&elements, element);

        if (_nya_serde_json_accept_symbol(parser, ',')) continue;
        if (_nya_serde_json_accept_symbol(parser, ']')) break;

        NYA_Token* token = _nya_serde_json_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated array");

        return nya_error(
            NYA_ERROR_PARSE,
            "expected ',' or ']' at line " FMTu32 ", got '%.*s'",
            token->line_number,
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    parser->depth--;

    *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = elements };
    return NYA_OK;
}

NYA_INTERNAL NYA_Error _nya_serde_json_parse_value(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value) {
    NYA_Token* token = _nya_serde_json_peek(parser);
    if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "unexpected end of input where a value was expected");

    const char* source = parser->lexer->source + token->source_location;

    if (token->type == NYA_TOKEN_STRING) {
        NYA_CString text = nullptr;
        NYA_TRY(_nya_serde_json_parse_string(parser, &text));

        *out_value = (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = text };
        return NYA_OK;
    }

    if (token->type == NYA_TOKEN_NUMBER_INTEGER || token->type == NYA_TOKEN_NUMBER_FLOAT) return _nya_serde_json_parse_number(parser, out_value);

    if (token->type == NYA_TOKEN_SYMBOL) {
        if (token->symbol == '-') return _nya_serde_json_parse_number(parser, out_value);

        if (token->symbol == '{') {
            NYA_Object* nested = nullptr;
            NYA_TRY(_nya_serde_json_parse_object(parser, &nested));

            *out_value = (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested };
            return NYA_OK;
        }

        if (token->symbol == '[') {
            parser->index++;
            return _nya_serde_json_parse_array(parser, out_value);
        }
    }

    if (token->type == NYA_TOKEN_IDENT) {
        if (_nya_serde_json_token_equals(parser, token, "true")) {
            parser->index++;
            *out_value = (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true };
            return NYA_OK;
        }

        if (_nya_serde_json_token_equals(parser, token, "false")) {
            parser->index++;
            *out_value = (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false };
            return NYA_OK;
        }

        if (_nya_serde_json_token_equals(parser, token, "null")) {
            parser->index++;
            *out_value = (NYA_Value){ .type = NYA_TYPE_NULL };
            return NYA_OK;
        }
    }

    return nya_error(
        NYA_ERROR_PARSE,
        "unexpected '%.*s' at line " FMTu32 " where a value was expected",
        (int)token->length,
        source,
        token->line_number
    );
}

/**
 * Reads a JSON number.
 *
 * An integer literal becomes s64 and anything with a fraction or an exponent becomes f64. JSON
 * cannot say which was meant, so this is the guess documented in the header. An integer too large
 * for s64 falls back to f64 rather than failing, matching what every other JSON reader does.
 * */
NYA_INTERNAL NYA_Error _nya_serde_json_parse_number(_NYA_SerdeJsonParser* parser, OUT NYA_Value* out_value) {
    NYA_Token* token    = _nya_serde_json_peek(parser);
    b8         negative = token->type == NYA_TOKEN_SYMBOL && token->symbol == '-';

    if (negative) {
        parser->index++;

        token = _nya_serde_json_peek(parser);
        if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "expected a number after '-'");
    }

    if (token->type != NYA_TOKEN_NUMBER_INTEGER && token->type != NYA_TOKEN_NUMBER_FLOAT) {
        return nya_error(
            NYA_ERROR_PARSE,
            "expected a number at line " FMTu32 ", got '%.*s'",
            token->line_number,
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    char text[192];
    u64  length = 0;

    if (negative) text[length++] = '-';

    u64 digits_length = token->length;
    if (digits_length > sizeof(text) - length - 1) digits_length = sizeof(text) - length - 1;

    nya_memcpy(text + length, parser->lexer->source + token->source_location, digits_length);
    length       += digits_length;
    text[length]  = '\0';

    parser->index++;

    if (token->type == NYA_TOKEN_NUMBER_INTEGER) {
        s64 integer = 0;
        if (nya_type_parse(NYA_TYPE_S64, (const u8*)text, length, &integer)) {
            *out_value = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = integer };
            return NYA_OK;
        }
    }

    f64 real = 0.0;
    if (!nya_type_parse(NYA_TYPE_F64, (const u8*)text, length, &real)) return nya_error(NYA_ERROR_PARSE, "'%s' is not a valid number", text);

    *out_value = (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = real };
    return NYA_OK;
}

/** Reads a quoted string and unescapes it, including surrogate paired \\u escapes. */
NYA_INTERNAL NYA_Error _nya_serde_json_parse_string(_NYA_SerdeJsonParser* parser, OUT NYA_CString* out_text) {
    NYA_Token* token = _nya_serde_json_peek(parser);
    if (token == nullptr) return nya_error(NYA_ERROR_PARSE, "expected a string but the document ended");

    if (token->type != NYA_TOKEN_STRING) {
        return nya_error(
            NYA_ERROR_PARSE,
            "expected a string at line " FMTu32 ", got '%.*s'",
            token->line_number,
            (int)token->length,
            parser->lexer->source + token->source_location
        );
    }

    const char* source = parser->lexer->source + token->source_location;

    // A \u escape is 6 source characters and yields at most 3 UTF-8 bytes, and a surrogate pair is
    // 12 source characters for 4 bytes, so unescaping never grows the text. The raw length plus a
    // terminator is always enough.
    NYA_CString unescaped = nya_arena_alloc(parser->arena, token->length + 1);
    u64         written   = 0;

    for (u64 i = 0; i < token->length; i++) {
        if (source[i] != '\\') {
            unescaped[written++] = source[i];
            continue;
        }

        i++;
        if (i >= token->length) return nya_error(NYA_ERROR_PARSE, "string ends in a trailing backslash");

        switch (source[i]) {
            case '"':  unescaped[written++] = '"'; break;
            case '\\': unescaped[written++] = '\\'; break;
            case '/':  unescaped[written++] = '/'; break;
            case 'b':  unescaped[written++] = '\b'; break;
            case 'f':  unescaped[written++] = '\f'; break;
            case 'n':  unescaped[written++] = '\n'; break;
            case 'r':  unescaped[written++] = '\r'; break;
            case 't':  unescaped[written++] = '\t'; break;

            case 'u':  {
                if (i + 4 >= token->length) return nya_error(NYA_ERROR_PARSE, "truncated \\u escape");

                u32 codepoint = 0;
                for (u32 digit = 1; digit <= 4; digit++) {
                    char hex   = source[i + digit];
                    u32  value = 0;

                    if ('0' <= hex && hex <= '9')
                        value = (u32)(hex - '0');
                    else if ('a' <= hex && hex <= 'f')
                        value = (u32)(hex - 'a') + 10;
                    else if ('A' <= hex && hex <= 'F')
                        value = (u32)(hex - 'A') + 10;
                    else
                        return nya_error(NYA_ERROR_PARSE, "'%c' is not a hex digit in a \\u escape", hex);

                    codepoint = (codepoint << 4) | value;
                }
                i += 4;

                // A codepoint above the BMP arrives as a surrogate pair. Combine them, otherwise
                // the result is two unpaired surrogates, which are not valid UTF-8.
                if (0xD800 <= codepoint && codepoint <= 0xDBFF && i + 6 < token->length && source[i + 1] == '\\' && source[i + 2] == 'u') {
                    u32 low = 0;
                    b8  ok  = true;

                    for (u32 digit = 3; digit <= 6; digit++) {
                        char hex   = source[i + digit];
                        u32  value = 0;

                        if ('0' <= hex && hex <= '9')
                            value = (u32)(hex - '0');
                        else if ('a' <= hex && hex <= 'f')
                            value = (u32)(hex - 'a') + 10;
                        else if ('A' <= hex && hex <= 'F')
                            value = (u32)(hex - 'A') + 10;
                        else {
                            ok = false;
                            break;
                        }

                        low = (low << 4) | value;
                    }

                    if (ok && 0xDC00 <= low && low <= 0xDFFF) {
                        codepoint  = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        i         += 6;
                    }
                }

                written += _nya_serde_json_encode_utf8(codepoint, unescaped + written);
            } break;

            default: return nya_error(NYA_ERROR_PARSE, "unknown escape '\\%c'", source[i]);
        }
    }

    unescaped[written] = '\0';
    parser->index++;

    *out_text = unescaped;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * TOKEN HELPERS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Token* _nya_serde_json_peek(_NYA_SerdeJsonParser* parser) {
    if (parser->index >= parser->lexer->tokens->length) return nullptr;

    NYA_Token* token = &parser->lexer->tokens->items[parser->index];
    return token->type == NYA_TOKEN_EOF ? nullptr : token;
}

NYA_INTERNAL b8 _nya_serde_json_accept_symbol(_NYA_SerdeJsonParser* parser, char symbol) {
    NYA_Token* token = _nya_serde_json_peek(parser);
    if (token == nullptr || token->type != NYA_TOKEN_SYMBOL || token->symbol != (u8)symbol) return false;

    parser->index++;
    return true;
}

NYA_INTERNAL b8 _nya_serde_json_token_equals(_NYA_SerdeJsonParser* parser, const NYA_Token* token, NYA_ConstCString text) {
    u64 length = strlen(text);
    if (token->length != length) return false;

    return strncmp(parser->lexer->source + token->source_location, text, length) == 0;
}

/** Writes `codepoint` as UTF-8 and returns how many bytes it took. */
NYA_INTERNAL u32 _nya_serde_json_encode_utf8(u32 codepoint, OUT char* out) {
    if (codepoint <= 0x7F) {
        out[0] = (char)codepoint;
        return 1;
    }

    if (codepoint <= 0x7FF) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }

    if (codepoint <= 0xFFFF) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}
