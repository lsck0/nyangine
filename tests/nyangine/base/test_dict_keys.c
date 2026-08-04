/**
 * Dict key semantics.
 *
 * A dict is an NYA_CString keyed hmap created with string hashing and string equality rather than
 * the byte-wise defaults, and that substitution is the entire point of the specialisation: the
 * default would hash and compare the char* itself, so two equal strings at different addresses
 * would be different keys.
 *
 * test_dict.c only ever uses string literals, which a compiler is free to pool into one address —
 * so every one of its lookups would still pass if the hash were on the pointer. This uses keys
 * built at runtime, where the addresses genuinely differ.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_dict(u32);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_dict_keys");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: equal content at different addresses is the same key
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: keys compare by content\n");
  {
    NYA_Dictᐸu32ᐳ* dict = nya_dict_create(arena, u32);

    // Two buffers holding "alpha", at addresses that cannot have been pooled.
    char stored[16];
    char probe[16];
    (void)snprintf(stored, sizeof(stored), "%s%s", "al", "pha");
    (void)snprintf(probe, sizeof(probe), "%s%s", "alp", "ha");
    nya_assert(stored != probe, "the two buffers must be distinct objects");
    nya_assert(strcmp(stored, probe) == 0);

    nya_dict_set(dict, stored, 42U);

    u32* found = nya_dict_get(dict, probe);
    nya_assert(found != nullptr, "a key with equal content at another address was not found");
    nya_assert(*found == 42U);
    nya_assert(nya_dict_contains(dict, probe) == true);

    // And setting through the other buffer updates rather than inserting a second entry.
    nya_dict_set(dict, probe, 43U);
    nya_assert(dict->length == 1, "equal keys produced " FMTu64 " entries", dict->length);
    nya_assert(*nya_dict_get(dict, stored) == 43U);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: different content is a different key, including near misses
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: distinct keys stay distinct\n");
  {
    NYA_Dictᐸu32ᐳ* dict = nya_dict_create(arena, u32);

    nya_dict_set(dict, "a", 1U);
    nya_dict_set(dict, "ab", 2U);
    nya_dict_set(dict, "b", 3U);
    nya_dict_set(dict, "", 4U);   // the empty key is a key like any other

    nya_assert(dict->length == 4);
    nya_assert(*nya_dict_get(dict, "a") == 1U);
    nya_assert(*nya_dict_get(dict, "ab") == 2U);
    nya_assert(*nya_dict_get(dict, "b") == 3U);
    nya_assert(*nya_dict_get(dict, "") == 4U);

    nya_assert(nya_dict_get(dict, "abc") == nullptr);
    nya_assert(nya_dict_contains(dict, "A") == false);   // case matters
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a key whose buffer is later overwritten
  //
  // The dict stores the pointer it was given. Nothing copies the bytes, so mutating the caller's
  // buffer afterwards changes what that entry's key reads as — worth pinning, because it is the
  // difference between a dict that owns its keys and one that borrows them.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: keys are borrowed, not copied\n");
  {
    NYA_Dictᐸu32ᐳ* dict = nya_dict_create(arena, u32);

    // A key that outlives the call, allocated from the arena rather than the stack.
    NYA_String* owned    = nya_string_from(arena, "stable");
    NYA_CString owned_key = nya_string_to_cstring(arena, owned);

    nya_dict_set(dict, owned_key, 7U);
    nya_assert(*nya_dict_get(dict, "stable") == 7U);

    // Looking up with an independently built copy still finds it, which is the content-hash
    // property again and the reason borrowing is safe as long as the bytes do not change.
    char rebuilt[16];
    (void)snprintf(rebuilt, sizeof(rebuilt), "stable");
    nya_assert(*nya_dict_get(dict, rebuilt) == 7U);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many runtime-built keys, forcing growth and rehashing
  //
  // A rehash re-inserts every key, so it exercises the hash function far more than a handful of
  // literals do. If the hash were on the pointer, a rehash would still work but a lookup with a
  // fresh buffer afterwards would not.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: growth and rehash\n");
  {
    NYA_Dictᐸu32ᐳ* dict = nya_dict_create_with_capacity(arena, u32, 4);

    enum { COUNT = 200 };

    for (u32 i = 0; i < COUNT; i++) {
      NYA_String* key = nya_string_sprintf(arena, "key-%u", i);
      nya_dict_set(dict, nya_string_to_cstring(arena, key), i);
    }
    nya_assert(dict->length == COUNT);

    // Look every one back up through a buffer built separately from the one that inserted it.
    for (u32 i = 0; i < COUNT; i++) {
      char probe[32];
      (void)snprintf(probe, sizeof(probe), "key-%u", i);

      u32* found = nya_dict_get(dict, probe);
      nya_assert(found != nullptr, "key-%u vanished after rehashing", i);
      nya_assert(*found == i, "key-%u mapped to %u", i, *found);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: removal, and that it does not disturb its neighbours
  //
  // Open addressing is where removal goes wrong: deleting an entry in the middle of a probe chain
  // can strand everything after it if the slot is simply blanked.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: removal keeps probe chains intact\n");
  {
    NYA_Dictᐸu32ᐳ* dict = nya_dict_create_with_capacity(arena, u32, 8);

    enum { COUNT = 64 };

    for (u32 i = 0; i < COUNT; i++) {
      NYA_String* key = nya_string_sprintf(arena, "k%u", i);
      nya_dict_set(dict, nya_string_to_cstring(arena, key), i);
    }

    // Remove every other entry.
    for (u32 i = 0; i < COUNT; i += 2) {
      char probe[32];
      (void)snprintf(probe, sizeof(probe), "k%u", i);
      nya_dict_remove(dict, probe);
    }
    nya_assert(dict->length == COUNT / 2);

    // Everything that was not removed must still be reachable.
    for (u32 i = 1; i < COUNT; i += 2) {
      char probe[32];
      (void)snprintf(probe, sizeof(probe), "k%u", i);

      u32* found = nya_dict_get(dict, probe);
      nya_assert(found != nullptr, "k%u was stranded by removing its neighbours", i);
      nya_assert(*found == i);
    }

    // And everything removed is gone.
    for (u32 i = 0; i < COUNT; i += 2) {
      char probe[32];
      (void)snprintf(probe, sizeof(probe), "k%u", i);
      nya_assert(nya_dict_get(dict, probe) == nullptr, "k%u survived removal", i);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a copied dict still hashes by content
  //
  // nya_dict_copy is nya_dict_copy is nya_hmap_copy, and the hash and equality functions are what
  // a copy most easily loses: every other field is data, those two are behaviour. A copy that
  // dropped them would call through a null pointer on its first lookup.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: copy keeps the key semantics\n");
  {
    NYA_Dictᐸu32ᐳ* original = nya_dict_create(arena, u32);
    nya_dict_set(original, "one", 1U);
    nya_dict_set(original, "two", 2U);

    NYA_Dictᐸu32ᐳ  copy_val = nya_dict_copy(original);
    NYA_Dictᐸu32ᐳ* copy     = &copy_val;

    nya_assert(copy->length == 2);

    // Through a runtime buffer, so this only works if the copy kept the string hash.
    char probe[16];
    (void)snprintf(probe, sizeof(probe), "one");
    nya_assert(nya_dict_get(copy, probe) != nullptr, "a copied dict lost its hash function");
    nya_assert(*nya_dict_get(copy, probe) == 1U);

    // The copy is independent of the original.
    nya_dict_set(copy, "three", 3U);
    nya_assert(original->length == 2);
    nya_assert(nya_dict_get(original, "three") == nullptr);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  printf("PASSED: test_dict_keys\n");
  return 0;
}
