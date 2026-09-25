/**
 * The stored password hash, parsed from whatever bytes are in the column.
 *
 * The encoded hash is `$argon2id$v=19$m=...,t=...,p=...$salt$hash`, and a database row is a thing an
 * attacker may one day get to write — a restored backup, another service's table, a migration that went
 * sideways. So the parser is fed arbitrary bytes, and the oracle is that it reaches a clean yes-or-no for
 * every one: no read past the end of the string, no assert, and no cost misread as a smaller number than
 * the digits said, which would be a hash verified at a cost the attacker chose.
 *
 * A string it refuses is the expected answer to garbage and is not a finding. The sanitizers are the
 * oracle for the memory; the assertions below are the oracle for the meaning.
 **/

// clang-format off
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"
// clang-format on

#define FUZZ_TARGET "password_hash"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_password_hash");
    defer      nya_arena_destroy(arena);

    // The parser walks a C string, so the input is copied and terminated. An embedded NUL simply ends the string early, which the strlen/strncmp/strrchr walk must handle without stepping past it.
    char* encoded = nya_arena_alloc(arena, size + 1);
    if (size > 0) nya_memcpy(encoded, data, size);
    encoded[size] = '\0';

    // The hand-written cost reader: total, and when it accepts, three non-zero parameters that each fit a u32. A misread here is a cost this process would then hash at.
    u32 memory_kib = 0xDEAD;
    u32 passes     = 0xBEEF;
    u32 lanes      = 0xCAFE;

    b8 parsed = _nya_account_password_cost(encoded, &memory_kib, &passes, &lanes);

    if (parsed) {
        nya_assert(memory_kib > 0 && passes > 0 && lanes > 0, "an accepted hash parsed to a zero parameter");
    } else {
        nya_assert(memory_kib == 0 && passes == 0 && lanes == 0, "a refused hash left a half-read cost behind");
    }

    // needs_rehash is the public door onto the same parser, and must be total for any column bytes: a hash it cannot read is one worth replacing, never a crash. It reads the fixed-width column, so the input is placed there truncated the way a real row would be.
    NYA_AccountUser user = { 0 };
    (void)snprintf(user.password, sizeof(user.password), "%s", encoded);
    b8 needs = nya_account_password_needs_rehash(&user);
    nya_unused(needs);

    // Verify reaches the deeper parser — the salt and hash base64url decode — and returns false long before the expensive Argon2id whenever the shape is wrong, which for arbitrary bytes is always. It must refuse cleanly rather than read past a field it could not find.
    b8 verified = _nya_account_password_verify("a candidate password", encoded);
    nya_unused(verified);
}

#include "tests/fuzz/fuzz.h"
