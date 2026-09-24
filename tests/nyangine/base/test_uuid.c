/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // TEST: UUID constants
  nya_assert(NYA_UUID_STRING_LENGTH == 36);
  nya_assert(NYA_UUID_STRING_BUFFER_SIZE == 37);

  // TEST: v4 UUID creation
  NYA_RNG  rng  = nya_rng_create();
  NYA_Uuid v4_1 = nya_uuid_v4_create(&rng);
  NYA_Uuid v4_2 = nya_uuid_v4_create(&rng);

  nya_assert(v4_1 != 0);
  nya_assert(v4_2 != 0);
  nya_assert(v4_1 != v4_2);

  // TEST: v7 UUID creation
  NYA_Uuid v7_1 = nya_uuid_v7_create(&rng);
  NYA_Uuid v7_2 = nya_uuid_v7_create(&rng);

  nya_assert(v7_1 != 0);
  nya_assert(v7_2 != 0);
  nya_assert(v7_1 != v7_2);

  // TEST: UUID format
  char buffer[NYA_UUID_STRING_BUFFER_SIZE];

  nya_uuid_format(0, buffer);
  nya_assert(nya_string_equals(buffer, "00000000-0000-0000-0000-000000000000"));

  nya_uuid_format(v4_1, buffer);
  nya_assert(nya_string_starts_with(buffer + 13, "-4"));
  nya_assert(buffer[19] >= '8' && buffer[19] <= 'b');

  nya_uuid_format(v7_1, buffer);
  nya_assert(nya_string_starts_with(buffer + 13, "-7"));
  nya_assert(buffer[19] >= '8' && buffer[19] <= 'b');

  // Verify format length
  nya_assert(nya_string_equals(buffer + 36, ""));

  // TEST: UUID parse
  NYA_Uuid parsed;

  b8 ok = nya_uuid_parse("550e8400-e29b-41d4-a716-446655440000", &parsed);
  nya_assert(ok);
  nya_assert(parsed == ((((u128)0x00004455664416a7ULL << 64) | (u128)0xd4419be200840e55ULL)));

  ok = nya_uuid_parse("550e8400e29b41d4a716446655440000", &parsed);
  nya_assert(ok);
  nya_assert(parsed == ((((u128)0x00004455664416a7ULL << 64) | (u128)0xd4419be200840e55ULL)));

  ok = nya_uuid_parse("00000000-0000-0000-0000-000000000000", &parsed);
  nya_assert(ok);
  nya_assert(parsed == 0);

  ok = nya_uuid_parse("00000000000000000000000000000000", &parsed);
  nya_assert(ok);
  nya_assert(parsed == 0);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-4466554400000", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000g", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000-", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("", &parsed);
  nya_assert(!ok);

  // TEST: Roundtrip format/parse v4
  {
    NYA_Uuid original = nya_uuid_v4_create(&rng);
    char     str[NYA_UUID_STRING_BUFFER_SIZE];
    nya_uuid_format(original, str);

    NYA_Uuid roundtripped;
    ok = nya_uuid_parse(str, &roundtripped);
    nya_assert(ok);
    nya_assert(original == roundtripped);
  }

  // TEST: Roundtrip format/parse v7
  {
    NYA_Uuid original = nya_uuid_v7_create(&rng);
    char     str[NYA_UUID_STRING_BUFFER_SIZE];
    nya_uuid_format(original, str);

    NYA_Uuid roundtripped;
    ok = nya_uuid_parse(str, &roundtripped);
    nya_assert(ok);
    nya_assert(original == roundtripped);
  }

  // TEST: UUID parse invalid characters
  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000z", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000/", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4-a716-44665544000:", &parsed);
  nya_assert(!ok);

  // TEST: UUID parse with wrong hyphen positions
  ok = nya_uuid_parse("550e840-0e29b-41d4-a716-446655440000", &parsed);
  nya_assert(!ok);

  ok = nya_uuid_parse("550e8400-e29b-41d4a716-446655440000", &parsed);
  nya_assert(!ok);

  return 0;
}
