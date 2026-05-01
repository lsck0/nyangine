#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_OBJECT_SERIALIZATION_VERSION 1
#define _NYA_OBJECT_INDENT_SIZE           4
#define _NYA_OBJECT_MINIFY_MAGIC          0xA7

/** "nyangine" */
NYA_INTERNAL const u8 _NYA_OBJECT_XOR_KEY[] = {
  0x6E, 0x79, 0x61, 0x6E, 0x67, 0x69, 0x6E, 0x65,
};

NYA_INTERNAL void _nya_object_serialize_object(NYA_String* out, const NYA_Value* value, u32 indent, NYA_ObjectSerializationFlags flags);
NYA_INTERNAL void _nya_object_serialize_value(NYA_String* out, const NYA_Value* value, u32 indent, NYA_ObjectSerializationFlags flags);
NYA_INTERNAL void _nya_object_append_indent(NYA_String* str, u32 indent);

NYA_INTERNAL NYA_Result _nya_object_parse_kvs(NYA_Arena* arena, NYA_Lexer* lexer, u64* idx, NYA_Object* obj);
NYA_INTERNAL NYA_Result _nya_object_parse_value(NYA_Arena* arena, NYA_Lexer* lexer, u64* idx, NYA_Type type, OUT NYA_Value* out_value);
NYA_INTERNAL void       _nya_object_skip_comments(NYA_Lexer* lexer, u64* idx);
NYA_INTERNAL b8         _nya_object_try_consume_semicolon(NYA_Lexer* lexer, u64* idx);
NYA_INTERNAL b8         _nya_object_try_consume_array_brackets(NYA_Lexer* lexer, u64* idx);

NYA_INTERNAL void _nya_object_xor(u8* data, u64 len);
NYA_INTERNAL u64  _nya_object_crc(const NYA_Object* obj);
NYA_INTERNAL u64  _nya_object_crc_value(const NYA_Value* value);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Object* nya_object_create(NYA_Arena* arena) {
  nya_assert(arena != nullptr);
  return nya_dict_create(arena, NYA_Value);
}

NYA_Object nya_object_create_on_stack(NYA_Arena* arena) {
  nya_assert(arena != nullptr);
  return nya_dict_create_on_stack(arena, NYA_Value);
}

void nya_object_reset(NYA_Object* obj) {
  nya_assert(obj != nullptr);
  nya_dict_clear(obj);
}

void nya_object_destroy(NYA_Object* obj) {
  nya_assert(obj != nullptr);
  nya_dict_destroy(obj);
}

void nya_object_destroy_on_stack(NYA_Object* obj) {
  nya_assert(obj != nullptr);
  nya_dict_destroy_on_stack(*obj);
}

NYA_Value* nya_object_get(const NYA_Object* obj, NYA_CString key) {
  nya_assert(obj != nullptr);
  nya_assert(key != nullptr);
  return nya_dict_get(obj, key);
}

void nya_object_set(NYA_Object* obj, NYA_CString key, NYA_Value value) {
  nya_assert(obj != nullptr);
  nya_assert(key != nullptr);
  nya_dict_set(obj, key, value);
}

void nya_object_remove(NYA_Object* obj, NYA_CString key) {
  nya_assert(obj != nullptr);
  nya_assert(key != nullptr);
  nya_dict_remove(obj, key);
}

NYA_String* nya_object_serialize(NYA_Arena* arena, const NYA_Object* obj, NYA_ObjectSerializationFlags flags) {
  nya_assert(arena != nullptr);
  nya_assert(obj != nullptr);

  b8 pretty      = nya_flag_check(flags, NYA_OBJECT_SERIALIZATION_FLAG_PRETTY);
  b8 minify      = nya_flag_check(flags, NYA_OBJECT_SERIALIZATION_FLAG_MINIFY);
  b8 use_compact = minify || !pretty;

  NYA_String* result = nya_string_create_with_capacity(arena, 512);

  // header
  nya_string_extend_sprintf(result, FMTs32 " " FMTu64, _NYA_OBJECT_SERIALIZATION_VERSION, _nya_object_crc(obj));
  nya_string_extend(result, use_compact ? " " : "\n");

  // object
  NYA_Value obj_value = {
    .type      = NYA_TYPE_OBJECT,
    .as_object = *obj,
  };
  _nya_object_serialize_object(result, &obj_value, 0, (!use_compact ? NYA_OBJECT_SERIALIZATION_FLAG_PRETTY : NYA_OBJECT_SERIALIZATION_FLAG_NONE));

  // base64 encode and XOR obfuscate if minify flag
  if (minify) {
    NYA_Arena*  serialize_arena = nya_arena_create();
    NYA_String* encoded         = nya_string_create(serialize_arena);

    nya_base64_encode(encoded, result->items, result->length);
    _nya_object_xor(encoded->items, encoded->length);

    char magic_byte_buffer[2] = { (char)_NYA_OBJECT_MINIFY_MAGIC, '\0' };
    nya_string_extend_front(encoded, magic_byte_buffer);

    nya_string_clear(result);
    nya_string_extend(result, encoded);

    nya_arena_destroy(serialize_arena);
  }

  return result;
}

NYA_Result nya_object_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_ObjectSerializationFlags flags, OUT NYA_Object** out_obj) {
  nya_assert(arena != nullptr);
  nya_assert(out_obj != nullptr);

  *out_obj = nullptr;
  if (size == 0 || data == nullptr) return NYA_OK;

  b8 skip_crc    = nya_flag_check(flags, NYA_OBJECT_SERIALIZATION_FLAG_NO_CRC_VALIDATION);
  b8 is_minified = (size > 0 && data[0] == _NYA_OBJECT_MINIFY_MAGIC);

  char* text;

  if (is_minified) {
    // skip magic byte, XOR to recover base64, then decode
    const u8* payload     = data + 1;
    u64       payload_len = size - 1;

    u8* xor_buf = nya_arena_alloc(arena, payload_len);
    nya_memcpy(xor_buf, payload, payload_len);
    _nya_object_xor(xor_buf, payload_len);

    NYA_String* decoded = nya_string_create(arena);
    nya_base64_decode(decoded, xor_buf, payload_len);

    text = nya_string_to_cstring(arena, decoded);

    nya_string_destroy(decoded);
    nya_arena_free(arena, xor_buf, payload_len);
  } else {
    text = nya_string_to_cstring(
        arena,
        &(NYA_String){
            .length = size,
            .items  = (u8*)data, // this is fine, were just copying it anyway
        }
    );
  }

  NYA_Lexer lexer = nya_lexer_create(text);
  nya_lexer_run(&lexer);

  u64 idx = 0;
  _nya_object_skip_comments(&lexer, &idx);

  // parse version
  NYA_Token* version_token = nya_array_get(lexer.tokens, idx);

  if (idx >= lexer.tokens->length || version_token->type != NYA_TOKEN_NUMBER_INTEGER) {
    return nya_err(NYA_ERROR_PARSE_ERROR, "missing or non-integer version");
  }

  s32 version;
  b8  ok = nya_type_parse(NYA_TYPE_S32, (u8*)lexer.source + version_token->source_location, version_token->length, &version);
  if (!ok) { return nya_err(NYA_ERROR_PARSE_ERROR, "version is not s32"); }

  if (version != _NYA_OBJECT_SERIALIZATION_VERSION) { return nya_err(NYA_ERROR_PARSE_ERROR, "unsupported version " FMTs32, version); }

  idx++;

  _nya_object_skip_comments(&lexer, &idx);

  // parse CRC
  NYA_Token* crc_token = nya_array_get(lexer.tokens, idx);

  if (idx >= lexer.tokens->length || crc_token->type != NYA_TOKEN_NUMBER_INTEGER) { return nya_err(NYA_ERROR_PARSE_ERROR, "missing or invalid CRC"); }

  u64 crc;
  ok = nya_type_parse(NYA_TYPE_U64, (u8*)lexer.source + crc_token->source_location, crc_token->length, &crc);
  if (!ok) { return nya_err(NYA_ERROR_PARSE_ERROR, "CRC is not u64"); }

  idx++;

  _nya_object_skip_comments(&lexer, &idx);

  // parse top-level object braces and key-value pairs
  if (idx >= lexer.tokens->length) { return nya_err(NYA_ERROR_PARSE_ERROR, "missing top-level object"); }

  NYA_Token* open_brace = nya_array_get(lexer.tokens, idx);
  if (!(open_brace->type == NYA_TOKEN_SYMBOL && open_brace->symbol == '{')) {
    return nya_err(NYA_ERROR_PARSE_ERROR, "expected '{' at the beginning of the object");
  }
  idx++;

  NYA_Object* obj = nya_object_create(arena);
  NYA_TRY(_nya_object_parse_kvs(arena, &lexer, &idx, obj));

  NYA_Token* close_brace = nya_array_get(lexer.tokens, idx);
  if (!(close_brace->type == NYA_TOKEN_SYMBOL && close_brace->symbol == '}')) {
    return nya_err(NYA_ERROR_PARSE_ERROR, "expected '}' at the end of the object");
  }

  // validate CRC
  if (!skip_crc) {
    u64 actual_crc = _nya_object_crc(obj);
    if (actual_crc != crc) { return nya_err(NYA_ERROR_PARSE_ERROR, "CRC mismatch (expected " FMTu64 ", got " FMTu64 ")", crc, actual_crc); }
  }

  *out_obj = obj;
  return NYA_OK;
}

NYA_String* nya_s128_to_string(NYA_Arena* arena, s128 value) {
  NYA_String* string = nya_string_create(arena);

  if (value == 0) nya_string_extend(string, "0");

  b8   is_negative = value < 0;
  u128 uvalue      = is_negative ? (u128)(-(value + 1)) + 1 : (u128)value;

  while (uvalue > 0) {
    u8 digit  = (u8)(uvalue % 10);
    uvalue   /= 10;

    nya_string_extend_front_sprintf(string, "%c", '0' + digit);
  }

  if (is_negative) nya_string_extend_front(string, "-");

  return string;
}

NYA_String* nya_u128_to_string(NYA_Arena* arena, u128 value) {
  NYA_String* string = nya_string_create(arena);

  if (value == 0) nya_string_extend(string, "0");

  while (value > 0) {
    u8 digit  = (u8)(value % 10);
    value    /= 10;

    nya_string_extend_front_sprintf(string, "%c", '0' + digit);
  }

  return string;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_object_serialize_object(NYA_String* out, const NYA_Value* value, u32 indent, NYA_ObjectSerializationFlags flags) {
  b8 pretty = nya_flag_check(flags, NYA_OBJECT_SERIALIZATION_FLAG_PRETTY);

  if (value->as_object.length == 0) {
    nya_string_extend(out, "{}");
    return;
  }

  nya_string_extend(out, pretty ? "{\n" : "{");

  b8 first = true;
  nya_dict_foreach_key(&value->as_object, key) {
    NYA_Value* v = nya_object_get(&value->as_object, *key);

    if (!first) nya_string_extend(out, pretty ? "\n" : " ");
    first = false;

    if (pretty) _nya_object_append_indent(out, indent + 1);

    nya_string_extend(out, *key);
    nya_string_extend(out, pretty ? ": " : ":");

    _nya_object_serialize_value(out, v, indent + 1, flags);
    nya_string_extend(out, ";");
  }

  if (pretty && !first) nya_string_extend(out, "\n");
  if (pretty) _nya_object_append_indent(out, indent);
  nya_string_extend(out, "}");
}

NYA_INTERNAL void _nya_object_serialize_value(NYA_String* out, const NYA_Value* value, u32 indent, NYA_ObjectSerializationFlags flags) {
  b8 pretty = nya_flag_check(flags, NYA_OBJECT_SERIALIZATION_FLAG_PRETTY);

  if (!(value->type == NYA_TYPE_ARRAY || nya_flag_check(flags, _NYA_OBJECT_SERIALIZATION_FLAG_NO_TYPE_SPECIFIER))) {
    nya_string_extend(out, NYA_TYPE_NAME_MAP[value->type]);
    nya_string_extend(out, " ");
  }

  switch (value->type) {
    case NYA_TYPE_NULL: nya_string_extend(out, "null"); break;

    case NYA_TYPE_BOOL: nya_string_extend(out, value->as_bool ? "true" : "false"); break;
    case NYA_TYPE_B8:   nya_string_extend_sprintf(out, FMTu8, value->as_b8); break;
    case NYA_TYPE_B16:  nya_string_extend_sprintf(out, FMTu16, value->as_b16); break;
    case NYA_TYPE_B32:  nya_string_extend_sprintf(out, FMTu32, value->as_b32); break;
    case NYA_TYPE_B64:  nya_string_extend_sprintf(out, FMTu64, value->as_b64); break;
    case NYA_TYPE_B128: {
      NYA_String* b128_str = nya_u128_to_string(out->arena, value->as_b128);
      nya_string_extend(out, b128_str);
      nya_string_destroy(b128_str);
    } break;

    case NYA_TYPE_U8:   nya_string_extend_sprintf(out, FMTu8, value->as_u8); break;
    case NYA_TYPE_U16:  nya_string_extend_sprintf(out, FMTu16, value->as_u16); break;
    case NYA_TYPE_U32:  nya_string_extend_sprintf(out, FMTu32, value->as_u32); break;
    case NYA_TYPE_U64:  nya_string_extend_sprintf(out, FMTu64, value->as_u64); break;
    case NYA_TYPE_U128: {
      NYA_String* u128_str = nya_u128_to_string(out->arena, value->as_u128);
      nya_string_extend(out, u128_str);
      nya_string_destroy(u128_str);
    } break;

    case NYA_TYPE_INTEGER: nya_string_extend_sprintf(out, FMTs64, value->as_s64); break;
    case NYA_TYPE_S8:      nya_string_extend_sprintf(out, FMTs8, value->as_s8); break;
    case NYA_TYPE_S16:     nya_string_extend_sprintf(out, FMTs16, value->as_s16); break;
    case NYA_TYPE_S32:     nya_string_extend_sprintf(out, FMTs32, value->as_s32); break;
    case NYA_TYPE_S64:     nya_string_extend_sprintf(out, FMTs64, value->as_s64); break;
    case NYA_TYPE_S128:    {
      NYA_String* s128_str = nya_s128_to_string(out->arena, value->as_s128);
      nya_string_extend(out, s128_str);
      nya_string_destroy(s128_str);
    } break;

    case NYA_TYPE_FLOAT:  nya_string_extend_sprintf(out, FMTf64, value->as_f64); break;
    case NYA_TYPE_F16:    nya_string_extend_sprintf(out, FMTf32, (f32)value->as_f16); break;
    case NYA_TYPE_F32:    nya_string_extend_sprintf(out, FMTf32, value->as_f32); break;
    case NYA_TYPE_F64:    nya_string_extend_sprintf(out, FMTf64, value->as_f64); break;
    case NYA_TYPE_F128:   nya_string_extend_sprintf(out, FMTf128, value->as_f128); break;

    case NYA_TYPE_CHAR:   nya_string_extend_sprintf(out, "%c", value->as_char); break;
    case NYA_TYPE_STRING: {
      nya_string_extend(out, "\"");
      for (const char* p = value->as_string; *p != '\0'; p++) {
        switch (*p) {
          case '"':  nya_string_extend(out, "\\\""); break;
          case '\\': nya_string_extend(out, "\\\\"); break;
          case '\n': nya_string_extend(out, "\\n"); break;
          case '\t': nya_string_extend(out, "\\t"); break;
          case '\r': nya_string_extend(out, "\\r"); break;
          default:   nya_string_extend_sprintf(out, "%c", *p); break;
        }
      }
      nya_string_extend(out, "\"");
    } break;

    case NYA_TYPE_OBJECT: {
      nya_flag_unset(flags, _NYA_OBJECT_SERIALIZATION_FLAG_NO_TYPE_SPECIFIER);
      _nya_object_serialize_object(out, value, indent, flags);
    } break;

    case NYA_TYPE_ARRAY: {
      if (value->as_array.length == 0) {
        nya_string_extend(out, "[]");
        break;
      }

      NYA_Type elem_type = value->as_array.items[0].type;
      nya_string_extend(out, NYA_TYPE_NAME_MAP[elem_type]);
      nya_string_extend(out, pretty ? "[] [\n" : "[] [");

      b8 first = true;
      nya_array_foreach (&value->as_array, elem) {
        if (!first) nya_string_extend(out, pretty ? ",\n" : ",");
        first = false;

        if (pretty) _nya_object_append_indent(out, indent + 1);

        nya_flag_set(flags, _NYA_OBJECT_SERIALIZATION_FLAG_NO_TYPE_SPECIFIER);
        _nya_object_serialize_value(out, elem, indent + 1, flags);
      }

      if (pretty && !first) nya_string_extend(out, ",\n");
      if (pretty) _nya_object_append_indent(out, indent);
      nya_string_extend(out, "]");
    } break;

    default: nya_panic("Unsupported value type for serialization: %s", NYA_TYPE_NAME_MAP[value->type]);
  }
}

NYA_INTERNAL void _nya_object_append_indent(NYA_String* str, u32 indent) {
  for (u32 i = 0; i < indent * _NYA_OBJECT_INDENT_SIZE; i++) nya_string_extend(str, " ");
}

NYA_INTERNAL NYA_Result _nya_object_parse_kvs(NYA_Arena* arena, NYA_Lexer* lexer, u64* idx, NYA_Object* obj) {
  while (*idx < lexer->tokens->length) {
    _nya_object_skip_comments(lexer, idx);
    if (*idx >= lexer->tokens->length) return NYA_OK;

    NYA_Token* itentifier = nya_array_get(lexer->tokens, *idx);
    if (itentifier->type == NYA_TOKEN_EOF) return NYA_OK;
    if (itentifier->type == NYA_TOKEN_SYMBOL && itentifier->symbol == '}') return NYA_OK;
    if (itentifier->type != NYA_TOKEN_IDENT) {
      return nya_err(NYA_ERROR_PARSE_ERROR, "expected identifier for key, got token type %d", itentifier->type);
    }

    char* key = nya_arena_alloc(arena, itentifier->length + 1);
    nya_memcpy(key, lexer->source + itentifier->source_location, itentifier->length);
    key[itentifier->length] = '\0';
    (*idx)++;

    if (*idx >= lexer->tokens->length) return nya_err(NYA_ERROR_PARSE_ERROR, "unexpected end of input after key '%s'", key);
    NYA_Token* colon = nya_array_get(lexer->tokens, *idx);
    if (!(colon->type == NYA_TOKEN_SYMBOL && colon->symbol == ':')) { return nya_err(NYA_ERROR_PARSE_ERROR, "expected ':' after key '%s'", key); }
    (*idx)++;

    if (*idx >= lexer->tokens->length) return nya_err(NYA_ERROR_PARSE_ERROR, "unexpected end of input after ':' for key '%s'", key);

    if (_nya_object_try_consume_array_brackets(lexer, idx)) {
      NYA_Value arr_value = { .type = NYA_TYPE_ARRAY, .as_array = nya_array_create_on_stack(arena, NYA_Value) };
      _nya_object_try_consume_semicolon(lexer, idx);
      nya_object_set(obj, key, arr_value);
      continue;
    }

    NYA_Token* type_token = &lexer->tokens->items[*idx];
    if (type_token->type != NYA_TOKEN_IDENT) { return nya_err(NYA_ERROR_PARSE_ERROR, "expected type name for key '%s'", key); }

    NYA_Type         value_type;
    NYA_ConstCString type_name;
    if (!nya_type_name_parse((const u8*)(lexer->source + type_token->source_location), type_token->length, &value_type, &type_name)) {
      return nya_err(
          NYA_ERROR_PARSE_ERROR,
          "unknown type '%.*s' for key '%s'",
          (int)type_token->length,
          lexer->source + type_token->source_location,
          key
      );
    }
    (*idx)++;

    b8 is_array = false;
    if (*idx < lexer->tokens->length) { is_array = _nya_object_try_consume_array_brackets(lexer, idx); }

    if (is_array) {
      NYA_Value      arr_value = { .type = NYA_TYPE_ARRAY };
      NYA_ValueArray arr       = nya_array_create_on_stack(arena, NYA_Value);

      if (*idx < lexer->tokens->length) {
        NYA_Token* open = &lexer->tokens->items[*idx];
        if (open->type == NYA_TOKEN_SYMBOL && open->symbol == '[') {
          (*idx)++;

          while (*idx < lexer->tokens->length) {
            NYA_Token* elem = &lexer->tokens->items[*idx];
            if (elem->type == NYA_TOKEN_SYMBOL && elem->symbol == ']') {
              (*idx)++;
              break;
            }
            if (elem->type == NYA_TOKEN_SYMBOL && elem->symbol == ',') {
              (*idx)++;
              continue;
            }

            NYA_Value v;
            NYA_TRY(_nya_object_parse_value(arena, lexer, idx, value_type, &v));
            nya_array_push_back(&arr, v);
          }
        }
      }

      arr_value.as_array = arr;
      nya_object_set(obj, key, arr_value);
    } else {
      NYA_Value v;
      NYA_TRY(_nya_object_parse_value(arena, lexer, idx, value_type, &v));
      nya_object_set(obj, key, v);
    }

    _nya_object_try_consume_semicolon(lexer, idx);
  }

  return NYA_OK;
}

NYA_INTERNAL NYA_Result _nya_object_parse_value(NYA_Arena* arena, NYA_Lexer* lexer, u64* idx, NYA_Type type, OUT NYA_Value* out_value) {
  NYA_Value value = { .type = type };

  if (*idx >= lexer->tokens->length) {
    return nya_err(NYA_ERROR_PARSE_ERROR, "unexpected end of input while parsing %s value", NYA_TYPE_NAME_MAP[type]);
  }

  NYA_Token*  token = &lexer->tokens->items[*idx];
  const char* str   = lexer->source + token->source_location;

  switch (type) {
    case NYA_TYPE_VOID:    break;

    case NYA_TYPE_BOOL:
    case NYA_TYPE_B8:
    case NYA_TYPE_U8:
    case NYA_TYPE_B16:
    case NYA_TYPE_U16:
    case NYA_TYPE_B32:
    case NYA_TYPE_U32:
    case NYA_TYPE_B64:
    case NYA_TYPE_U64:
    case NYA_TYPE_S8:
    case NYA_TYPE_S16:
    case NYA_TYPE_S32:
    case NYA_TYPE_S64:
    case NYA_TYPE_INTEGER:
    case NYA_TYPE_F16:
    case NYA_TYPE_F32:
    case NYA_TYPE_F64:
    case NYA_TYPE_FLOAT:
    case NYA_TYPE_CHAR:    {
      b8 parsed = false;
      if (token->type == NYA_TOKEN_SYMBOL && token->symbol == '-' && *idx + 1 < lexer->tokens->length) {
        NYA_Token* num = &lexer->tokens->items[*idx + 1];
        char       buf[128];
        buf[0]  = '-';
        u64 len = num->length < sizeof(buf) - 2 ? num->length : sizeof(buf) - 2;
        nya_memcpy(buf + 1, lexer->source + num->source_location, len);
        buf[len + 1] = '\0';
        if (nya_type_parse(type, (const u8*)buf, len + 1, &value.as_u8)) {
          *idx   += 2;
          parsed  = true;
        }
      } else {
        if (nya_type_parse(type, (const u8*)str, token->length, &value.as_u8)) {
          (*idx)++;
          parsed = true;
        }
      }
      if (!parsed) {
        return nya_err(NYA_ERROR_PARSE_ERROR, "failed to parse %s value from '%.*s'", NYA_TYPE_NAME_MAP[type], (int)token->length, str);
      }
    } break;

    case NYA_TYPE_STRING: {
      if (token->type != NYA_TOKEN_STRING) { return nya_err(NYA_ERROR_PARSE_ERROR, "expected quoted string, got '%.*s'", (int)token->length, str); }
      const char* raw     = lexer->source + token->source_location;
      u64         raw_len = token->length;
      char*       s       = nya_arena_alloc(arena, raw_len + 1);
      u64         j       = 0;
      for (u64 i = 0; i < raw_len; i++) {
        if (raw[i] == '\\' && i + 1 < raw_len) {
          i++;
          switch (raw[i]) {
            case '"':  s[j++] = '"'; break;
            case '\\': s[j++] = '\\'; break;
            case 'n':  s[j++] = '\n'; break;
            case 't':  s[j++] = '\t'; break;
            case 'r':  s[j++] = '\r'; break;
            default:
              s[j++] = '\\';
              s[j++] = raw[i];
              break;
          }
        } else {
          s[j++] = raw[i];
        }
      }
      s[j]            = '\0';
      value.as_string = s;
      (*idx)++;
    } break;

    case NYA_TYPE_OBJECT: {
      if (token->type == NYA_TOKEN_IDENT && strncmp(str, "object", token->length) == 0 && token->length == 6) {
        (*idx)++;
        if (*idx >= lexer->tokens->length) { return nya_err(NYA_ERROR_PARSE_ERROR, "unexpected end of input after 'object' keyword"); }
        token = &lexer->tokens->items[*idx];
      }

      if (!(token->type == NYA_TOKEN_SYMBOL && token->symbol == '{')) { return nya_err(NYA_ERROR_PARSE_ERROR, "expected '{' for object value"); }
      (*idx)++;

      NYA_Object* nested = nya_object_create(arena);
      NYA_TRY(_nya_object_parse_kvs(arena, lexer, idx, nested));

      if (*idx < lexer->tokens->length) {
        NYA_Token* close = &lexer->tokens->items[*idx];
        if (close->type == NYA_TOKEN_SYMBOL && close->symbol == '}') (*idx)++;
      }

      value.as_object = *nested;
    } break;

    default: return nya_err(NYA_ERROR_PARSE_ERROR, "unsupported type %s for deserialization", NYA_TYPE_NAME_MAP[type]);
  }

  *out_value = value;
  return NYA_OK;
}

NYA_INTERNAL void _nya_object_skip_comments(NYA_Lexer* lexer, u64* idx) {
  while (*idx + 1 < lexer->tokens->length) {
    NYA_Token* t0 = &lexer->tokens->items[*idx];
    NYA_Token* t1 = &lexer->tokens->items[*idx + 1];
    if (!(t0->type == NYA_TOKEN_SYMBOL && t0->symbol == '/' && t1->type == NYA_TOKEN_SYMBOL && t1->symbol == '*')) return;

    *idx += 2;
    while (*idx + 1 < lexer->tokens->length) {
      t0 = &lexer->tokens->items[*idx];
      t1 = &lexer->tokens->items[*idx + 1];

      if (t0->type == NYA_TOKEN_SYMBOL && t0->symbol == '*' && t1->type == NYA_TOKEN_SYMBOL && t1->symbol == '/') {
        *idx += 2;
        break;
      }

      (*idx)++;
    }
  }
}

NYA_INTERNAL b8 _nya_object_try_consume_array_brackets(NYA_Lexer* lexer, u64* idx) {
  if (*idx + 1 >= lexer->tokens->length) return false;

  NYA_Token* b1 = &lexer->tokens->items[*idx];
  NYA_Token* b2 = &lexer->tokens->items[*idx + 1];
  if (b1->type == NYA_TOKEN_SYMBOL && b1->symbol == '[' && b2->type == NYA_TOKEN_SYMBOL && b2->symbol == ']') {
    *idx += 2;
    return true;
  }

  return false;
}

NYA_INTERNAL b8 _nya_object_try_consume_semicolon(NYA_Lexer* lexer, u64* idx) {
  if (*idx >= lexer->tokens->length) return false;

  NYA_Token* semicolon = nya_array_get(lexer->tokens, *idx);
  if (semicolon->type == NYA_TOKEN_SYMBOL && semicolon->symbol == ';') {
    (*idx)++;
    return true;
  }

  return false;
}

NYA_INTERNAL void _nya_object_xor(u8* data, u64 len) {
  for (u64 i = 0; i < len; i++) data[i] ^= _NYA_OBJECT_XOR_KEY[i % sizeof(_NYA_OBJECT_XOR_KEY)];
}

NYA_INTERNAL u64 _nya_object_crc(const NYA_Object* obj) {
  u64 crc = 0;

  for (u64 i = 0; i < obj->capacity; i++) {
    if (!obj->occupied[i]) continue;

    u64 key_crc    = nya_crc64((const u8*)obj->keys[i], strlen(obj->keys[i]));
    u64 value_crc  = _nya_object_crc_value(&obj->values[i]);
    crc           ^= key_crc ^ value_crc;
  }

  return crc;
}

NYA_INTERNAL u64 _nya_object_crc_value(const NYA_Value* value) {
  NYA_Type type = value->type;
  u64      crc  = nya_crc64((const u8*)&type, sizeof(type));

  switch (type) {
    case NYA_TYPE_BOOL:    crc ^= nya_crc64((const u8*)&value->as_bool, sizeof(value->as_bool)); break;
    case NYA_TYPE_B8:      crc ^= nya_crc64(&value->as_b8, sizeof(value->as_b8)); break;
    case NYA_TYPE_U8:      crc ^= nya_crc64(&value->as_u8, sizeof(value->as_u8)); break;
    case NYA_TYPE_B16:     crc ^= nya_crc64((const u8*)&value->as_b16, sizeof(value->as_b16)); break;
    case NYA_TYPE_U16:     crc ^= nya_crc64((const u8*)&value->as_u16, sizeof(value->as_u16)); break;
    case NYA_TYPE_B32:     crc ^= nya_crc64((const u8*)&value->as_b32, sizeof(value->as_b32)); break;
    case NYA_TYPE_U32:     crc ^= nya_crc64((const u8*)&value->as_u32, sizeof(value->as_u32)); break;
    case NYA_TYPE_B64:     crc ^= nya_crc64((const u8*)&value->as_b64, sizeof(value->as_b64)); break;
    case NYA_TYPE_U64:     crc ^= nya_crc64((const u8*)&value->as_u64, sizeof(value->as_u64)); break;
    case NYA_TYPE_S8:      crc ^= nya_crc64((const u8*)&value->as_s8, sizeof(value->as_s8)); break;
    case NYA_TYPE_S16:     crc ^= nya_crc64((const u8*)&value->as_s16, sizeof(value->as_s16)); break;
    case NYA_TYPE_S32:     crc ^= nya_crc64((const u8*)&value->as_s32, sizeof(value->as_s32)); break;
    case NYA_TYPE_S64:
    case NYA_TYPE_INTEGER: crc ^= nya_crc64((const u8*)&value->as_s64, sizeof(value->as_s64)); break;
    case NYA_TYPE_F16:     crc ^= nya_crc64((const u8*)&value->as_f16, sizeof(value->as_f16)); break;
    case NYA_TYPE_F32:     crc ^= nya_crc64((const u8*)&value->as_f32, sizeof(value->as_f32)); break;
    case NYA_TYPE_F64:
    case NYA_TYPE_FLOAT:   crc ^= nya_crc64((const u8*)&value->as_f64, sizeof(value->as_f64)); break;
    case NYA_TYPE_B128:    crc ^= nya_crc64((const u8*)&value->as_b128, sizeof(value->as_b128)); break;
    case NYA_TYPE_U128:    crc ^= nya_crc64((const u8*)&value->as_u128, sizeof(value->as_u128)); break;
    case NYA_TYPE_S128:    crc ^= nya_crc64((const u8*)&value->as_s128, sizeof(value->as_s128)); break;
    case NYA_TYPE_F128:    crc ^= nya_crc64((const u8*)&value->as_f128, sizeof(value->as_f128)); break;
    case NYA_TYPE_CHAR:    crc ^= nya_crc64((const u8*)&value->as_char, sizeof(value->as_char)); break;
    case NYA_TYPE_STRING:  crc ^= nya_crc64((const u8*)value->as_string, strlen(value->as_string)); break;
    case NYA_TYPE_OBJECT:  crc ^= _nya_object_crc(&value->as_object); break;
    case NYA_TYPE_ARRAY:   {
      for (u64 i = 0; i < value->as_array.length; i++) {
        u64 elem_crc  = _nya_object_crc_value(&value->as_array.items[i]);
        u64 idx_crc   = nya_crc64((const u8*)&i, sizeof(i));
        crc          ^= elem_crc ^ idx_crc;
      }
    } break;

    default: nya_panic("Unsupported value type for CRC calculation: %s", NYA_TYPE_NAME_MAP[type]);
  }

  return crc;
}
