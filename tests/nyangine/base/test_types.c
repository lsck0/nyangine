/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Type limits - unsigned
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(U8_MIN == 0);
  nya_assert(U8_MAX == 255);
  nya_assert(U16_MIN == 0);
  nya_assert(U16_MAX == 65535);
  nya_assert(U32_MIN == 0);
  nya_assert(U32_MAX == 4294967295U);
  nya_assert(U64_MIN == 0);
  nya_assert(U64_MAX == 18446744073709551615ULL);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Type limits - signed
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(S8_MAX == 127);
  nya_assert(S8_MIN == -128);
  nya_assert(S16_MAX == 32767);
  nya_assert(S16_MIN == -32768);
  nya_assert(S32_MAX == 2147483647);
  nya_assert(S32_MIN == -2147483647 - 1);
  nya_assert(S64_MAX == 9223372036854775807LL);
  nya_assert(S64_MIN == -9223372036854775807LL - 1);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Type limits - float
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(F16_MAX == 65504.0F);
  nya_assert(F16_MIN == -65504.0F);
  nya_assert(F32_MAX > 0);
  nya_assert(F32_MIN < 0);
  nya_assert(F64_MAX > 0);
  nya_assert(F64_MIN < 0);
  nya_assert(F128_MAX > 0);
  nya_assert(F128_MIN < 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Safe cast - valid casts succeed
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_cast_to_u8(0) == 0);
  nya_assert(nya_cast_to_u8(255) == 255);
  nya_assert(nya_cast_to_u16(0) == 0);
  nya_assert(nya_cast_to_u16(65535) == 65535);
  nya_assert(nya_cast_to_u32(0) == 0);
  nya_assert(nya_cast_to_u32(4294967295U) == 4294967295U);
  nya_assert(nya_cast_to_u64(0) == 0);

  nya_assert(nya_cast_to_s8(-128) == -128);
  nya_assert(nya_cast_to_s8(127) == 127);
  nya_assert(nya_cast_to_s16(-32768) == -32768);
  nya_assert(nya_cast_to_s16(32767) == 32767);
  nya_assert(nya_cast_to_s32(-2147483647 - 1) == S32_MIN);
  nya_assert(nya_cast_to_s32(2147483647) == S32_MAX);
  nya_assert(nya_cast_to_s64(0) == 0);

  nya_assert(nya_cast_to_f32(0.0f) == 0.0f);
  nya_assert(nya_cast_to_f32(1.0f) == 1.0f);
  nya_assert(nya_cast_to_f64(0.0) == 0.0);
  nya_assert(nya_cast_to_f64(1.0) == 1.0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Safe cast - overflow panics
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_cast_to_u8(256));
  nya_expect_crash(nya_cast_to_u8(-1));
  nya_expect_crash(nya_cast_to_u16(65536));
  nya_expect_crash(nya_cast_to_u16(-1));
  nya_expect_crash(nya_cast_to_u32((s64)-1));

  nya_expect_crash(nya_cast_to_s8(128));
  nya_expect_crash(nya_cast_to_s8(-129));
  nya_expect_crash(nya_cast_to_s16(32768));
  nya_expect_crash(nya_cast_to_s16(-32769));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TYPE_NAME_MAP - key types have names
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_NULL], "null") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_VOID], "void") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_U8], "u8") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_U16], "u16") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_U32], "u32") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_U64], "u64") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_S8], "s8") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_S16], "s16") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_S32], "s32") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_S64], "s64") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_F32], "f32") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_F64], "f64") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_B8], "b8") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_STRING], "string") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_OBJECT], "object") == 0);
  nya_assert(strcmp(NYA_TYPE_NAME_MAP[NYA_TYPE_ARRAY], "array") == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TYPE_COUNT - enum count is consistent
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(NYA_TYPE_COUNT > 0);
  nya_assert(NYA_TYPE_NULL == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Type sizes
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(sizeof(u8) == 1);
  nya_assert(sizeof(u16) == 2);
  nya_assert(sizeof(u32) == 4);
  nya_assert(sizeof(u64) == 8);
  nya_assert(sizeof(s8) == 1);
  nya_assert(sizeof(s16) == 2);
  nya_assert(sizeof(s32) == 4);
  nya_assert(sizeof(s64) == 8);
  nya_assert(sizeof(f32) == 4);
  nya_assert(sizeof(f64) == 8);
  nya_assert(sizeof(b8) == 1);
  nya_assert(sizeof(b16) == 2);
  nya_assert(sizeof(b32) == 4);
  nya_assert(sizeof(b64) == 8);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_parse - unsigned integers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8  u8_val  = 0;
    u16 u16_val = 0;
    u32 u32_val = 0;
    u64 u64_val = 0;

    nya_assert(nya_type_parse(NYA_TYPE_U8, (u8*)"0", 1, &u8_val) == true);
    nya_assert(u8_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_U8, (u8*)"255", 3, &u8_val) == true);
    nya_assert(u8_val == 255);

    nya_assert(nya_type_parse(NYA_TYPE_U16, (u8*)"0", 1, &u16_val) == true);
    nya_assert(u16_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_U16, (u8*)"65535", 5, &u16_val) == true);
    nya_assert(u16_val == 65535);

    nya_assert(nya_type_parse(NYA_TYPE_U32, (u8*)"0", 1, &u32_val) == true);
    nya_assert(u32_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_U32, (u8*)"4294967295", 10, &u32_val) == true);
    nya_assert(u32_val == 4294967295U);

    nya_assert(nya_type_parse(NYA_TYPE_U64, (u8*)"0", 1, &u64_val) == true);
    nya_assert(u64_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_U64, (u8*)"18446744073709551615", 20, &u64_val) == true);
    nya_assert(u64_val == 18446744073709551615ULL);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_parse - signed integers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s8  s8_val  = 0;
    s16 s16_val = 0;
    s32 s32_val = 0;
    s64 s64_val = 0;

    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"0", 1, &s8_val) == true);
    nya_assert(s8_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"127", 3, &s8_val) == true);
    nya_assert(s8_val == 127);

    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"-128", 4, &s8_val) == true);
    nya_assert(s8_val == -128);

    nya_assert(nya_type_parse(NYA_TYPE_S16, (u8*)"0", 1, &s16_val) == true);
    nya_assert(s16_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_S16, (u8*)"32767", 5, &s16_val) == true);
    nya_assert(s16_val == 32767);

    nya_assert(nya_type_parse(NYA_TYPE_S16, (u8*)"-32768", 6, &s16_val) == true);
    nya_assert(s16_val == -32768);

    nya_assert(nya_type_parse(NYA_TYPE_S32, (u8*)"0", 1, &s32_val) == true);
    nya_assert(s32_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_S32, (u8*)"2147483647", 10, &s32_val) == true);
    nya_assert(s32_val == 2147483647);

    nya_assert(nya_type_parse(NYA_TYPE_S32, (u8*)"-2147483648", 11, &s32_val) == true);
    nya_assert(s32_val == -2147483647 - 1);

    nya_assert(nya_type_parse(NYA_TYPE_S64, (u8*)"0", 1, &s64_val) == true);
    nya_assert(s64_val == 0);

    nya_assert(nya_type_parse(NYA_TYPE_S64, (u8*)"9223372036854775807", 19, &s64_val) == true);
    nya_assert(s64_val == 9223372036854775807LL);

    nya_assert(nya_type_parse(NYA_TYPE_S64, (u8*)"-9223372036854775808", 20, &s64_val) == true);
    nya_assert(s64_val == -9223372036854775807LL - 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_parse - floats
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32 f32_val = 0.0f;
    f64 f64_val = 0.0;

    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"0", 1, &f32_val) == true);
    nya_assert(f32_val == 0.0f);

    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"123", 3, &f32_val) == true);
    nya_assert(f32_val == 123.0f);

    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"123.456", 7, &f32_val) == true);
    nya_assert(f32_val == 123.456f);

    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"0.5", 3, &f32_val) == true);
    nya_assert(f32_val == 0.5f);

    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"-123.456", 8, &f32_val) == true);
    nya_assert(f32_val == -123.456f);

    nya_assert(nya_type_parse(NYA_TYPE_F64, (u8*)"0", 1, &f64_val) == true);
    nya_assert(f64_val == 0.0);

    nya_assert(nya_type_parse(NYA_TYPE_F64, (u8*)"123.456", 7, &f64_val) == true);
    nya_assert(f64_val == 123.456);

    nya_assert(nya_type_parse(NYA_TYPE_F64, (u8*)"-123.456", 8, &f64_val) == true);
    nya_assert(f64_val == -123.456);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_parse - invalid input
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8 u8_val = 0;
    nya_assert(nya_type_parse(NYA_TYPE_U8, (u8*)"", 0, &u8_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_U8, (u8*)"abc", 3, &u8_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_U8, (u8*)"256", 3, &u8_val) == false);

    s8 s8_val = 0;
    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"", 0, &s8_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"-", 1, &s8_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"128", 3, &s8_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_S8, (u8*)"-129", 4, &s8_val) == false);

    f32 f32_val = 0.0f;
    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"", 0, &f32_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"abc", 3, &f32_val) == false);
    nya_assert(nya_type_parse(NYA_TYPE_F32, (u8*)"1..2", 4, &f32_val) == false);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_name_parse - valid type names
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Type         type = NYA_TYPE_NULL;
    NYA_ConstCString name = NULL;

    nya_assert(nya_type_name_parse((u8*)"u8", 2, &type, &name) == true);
    nya_assert(type == NYA_TYPE_U8);
    nya_assert(strcmp(name, "u8") == 0);

    nya_assert(nya_type_name_parse((u8*)"u16", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_U16);

    nya_assert(nya_type_name_parse((u8*)"u32", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_U32);

    nya_assert(nya_type_name_parse((u8*)"u64", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_U64);

    nya_assert(nya_type_name_parse((u8*)"s8", 2, &type, &name) == true);
    nya_assert(type == NYA_TYPE_S8);

    nya_assert(nya_type_name_parse((u8*)"s32", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_S32);

    nya_assert(nya_type_name_parse((u8*)"f32", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_F32);

    nya_assert(nya_type_name_parse((u8*)"f64", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_F64);

    nya_assert(nya_type_name_parse((u8*)"f16", 3, &type, &name) == true);
    nya_assert(type == NYA_TYPE_F16);

    nya_assert(nya_type_name_parse((u8*)"f128", 4, &type, &name) == true);
    nya_assert(type == NYA_TYPE_F128);

    nya_assert(nya_type_name_parse((u8*)"b8", 2, &type, &name) == true);
    nya_assert(type == NYA_TYPE_B8);

    nya_assert(nya_type_name_parse((u8*)"string", 6, &type, &name) == true);
    nya_assert(type == NYA_TYPE_STRING);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_type_name_parse - invalid type names
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Type         type = NYA_TYPE_NULL;
    NYA_ConstCString name = NULL;

    nya_assert(nya_type_name_parse((u8*)"", 0, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"invalid", 7, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"u7", 2, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"u9", 2, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"s7", 2, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"s9", 2, &type, &name) == false);
    nya_assert(nya_type_name_parse((u8*)"int", 3, &type, &name) == false);
  }

  return 0;
}
