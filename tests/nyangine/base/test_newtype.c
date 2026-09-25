/**
 * Parsed newtypes: base_newtype.h. For each of the three types the macro builds, a valid input constructs
 * and round-trips, a representative set of invalid inputs is each refused with an error, and equality
 * holds. Deterministic: the inputs are fixed strings, so the suite is the same run every time.
 *
 * Type distinctness — that NYA_Email, NYA_Username and NYA_UserId are not assignable to one another or to
 * a bare char* — is a compile-time property and cannot be asserted at runtime: the mismatch it guards
 * against would not compile. It is exercised here only by round-tripping each type on its own; the
 * guarantee itself lives in the fact that each is its own struct.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** Fills `buffer` with `count` copies of `fill` and a NUL, for the over-length cases. */
static void fill_string(char* buffer, char fill, u32 count) {
    for (u32 i = 0; i < count; i++) buffer[i] = fill;
    buffer[count] = '\0';
}

s32 main(void) {
    /* EMAIL */
    {
        // a valid address constructs and round-trips through the accessor.
        NYA_Email email = { 0 };
        NYA_Error result = nya_email_from_string("user@example.com", &email);
        nya_check(result.ok, "a valid email should parse");
        nya_check(email.length == 16, "email length should be 16, got %u", email.length);
        nya_check(nya_string_equals(nya_email_cstring(&email), "user@example.com"), "email should round-trip");

        // a dotted subdomain and digits in a label are fine.
        NYA_Email deep = { 0 };
        nya_check(nya_email_from_string("a.b+tag@mail.eu.example.com", &deep).ok, "a dotted email should parse");

        // each invalid address is refused with NYA_ERROR_INVALID_ARGUMENT.
        NYA_Email  scratch  = { 0 };
        const char* refused[] = {
            "",                       // empty
            "no-at-sign.example",     // no '@'
            "two@@example.com",       // two '@'
            "@example.com",           // empty local part
            "user@",                  // empty domain
            ".user@example.com",      // leading dot in local part
            "user.@example.com",      // trailing dot in local part
            "user..name@example.com", // doubled dot in local part
            "user@example",           // no dot in domain
            "user@example.",          // trailing dot in domain
            "user@-example.com",      // label begins with a hyphen
            "user@example.c0m",       // TLD is not all letters
            "user name@example.com",  // a space
        };
        for (u32 i = 0; i < nya_carray_length(refused); i++) {
            NYA_Error error = nya_email_from_string(refused[i], &scratch);
            nya_check(!error.ok && error.kind == NYA_ERROR_INVALID_ARGUMENT, "email '%s' should be refused", refused[i]);
        }

        // an address past the capacity is refused rather than truncated.
        char too_long[NYA_EMAIL_CAPACITY + 64];
        fill_string(too_long, 'a', NYA_EMAIL_CAPACITY + 32);
        nya_check(!nya_email_from_string(too_long, &scratch).ok, "an over-length email should be refused");

        // equality: same text equal, different text not.
        NYA_Email same = { 0 };
        (void)nya_email_from_string("user@example.com", &same);
        nya_check(nya_email_equals(&email, &same), "equal emails should compare equal");
        nya_check(!nya_email_equals(&email, &deep), "different emails should compare unequal");
    }

    /* USERNAME */
    {
        NYA_Username username = { 0 };
        NYA_Error    result   = nya_username_from_string("luca_42", &username);
        nya_check(result.ok, "a valid username should parse");
        nya_check(nya_string_equals(nya_username_cstring(&username), "luca_42"), "username should round-trip");

        NYA_Username scratch    = { 0 };
        const char*  refused[]  = {
            "",           // empty
            "ab",         // too short
            "_leading",   // begins with punctuation
            "has space",  // a space
            "at@sign",    // a disallowed character
            "café",       // non-ASCII
        };
        for (u32 i = 0; i < nya_carray_length(refused); i++) {
            NYA_Error error = nya_username_from_string(refused[i], &scratch);
            nya_check(!error.ok && error.kind == NYA_ERROR_INVALID_ARGUMENT, "username '%s' should be refused", refused[i]);
        }

        // over the length bound.
        char too_long[NYA_USERNAME_CAPACITY + 16];
        fill_string(too_long, 'x', NYA_USERNAME_CAPACITY + 4);
        nya_check(!nya_username_from_string(too_long, &scratch).ok, "an over-length username should be refused");

        // exactly at the bounds: three characters and the full capacity minus the NUL.
        NYA_Username shortest = { 0 };
        nya_check(nya_username_from_string("abc", &shortest).ok, "a three-character username should parse");
        char longest[NYA_USERNAME_CAPACITY];
        fill_string(longest, 'y', NYA_USERNAME_CAPACITY - 1);
        NYA_Username longest_name = { 0 };
        nya_check(nya_username_from_string(longest, &longest_name).ok, "a full-length username should parse");

        NYA_Username same = { 0 };
        (void)nya_username_from_string("luca_42", &same);
        nya_check(nya_username_equals(&username, &same), "equal usernames should compare equal");
        nya_check(!nya_username_equals(&username, &shortest), "different usernames should compare unequal");
    }

    /* USER ID */
    {
        NYA_UserId id     = { 0 };
        NYA_Error  result = nya_user_id_from_string("1729", &id);
        nya_check(result.ok, "a valid user id should parse");
        nya_check(nya_user_id_value(id) == 1729, "user id should be 1729, got " FMTu64, nya_user_id_value(id));

        // the largest u64 parses exactly.
        NYA_UserId max = { 0 };
        nya_check(nya_user_id_from_string("18446744073709551615", &max).ok, "U64_MAX should parse");
        nya_check(nya_user_id_value(max) == U64_MAX, "U64_MAX should round-trip");

        NYA_UserId  scratch   = { 0 };
        const char* refused[] = {
            "",                     // empty
            "0",                    // the null id is refused by the predicate
            "12a",                  // a non-digit
            "-5",                   // a sign is not a digit
            " 7",                   // leading whitespace is not a digit
            "18446744073709551616", // one past U64_MAX: overflow
            "99999999999999999999", // far past U64_MAX: overflow
        };
        for (u32 i = 0; i < nya_carray_length(refused); i++) {
            NYA_Error error = nya_user_id_from_string(refused[i], &scratch);
            nya_check(!error.ok && error.kind == NYA_ERROR_INVALID_ARGUMENT, "user id '%s' should be refused", refused[i]);
        }

        NYA_UserId same = { 0 };
        (void)nya_user_id_from_string("1729", &same);
        nya_check(nya_user_id_equals(id, same), "equal ids should compare equal");
        nya_check(!nya_user_id_equals(id, max), "different ids should compare unequal");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
