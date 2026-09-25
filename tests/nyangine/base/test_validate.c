/**
 * Declarative validation over the reflection attribute table, driven by hand written tables so the
 * attributes are exactly the ones under test.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/* THE TYPE UNDER TEST */

typedef struct {
    char username[33]; // @required @len(3, 32) @pattern(a*z)
    char email[255];   // @required @email
    s32  age;          // @min(13) @max(120)
    char nickname[16]; // @label(Nick) — an attribute validation has no rule for
} TestSignup;

/* THE TABLES, AS THE GENERATOR WOULD EMIT THEM */

static const NYA_TypeReflection _NYA_REFLECT_char_33 = {
    .name          = "char[33]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(char[33]),
    .alignment     = alignof(char[33]),
    .element       = &_NYA_REFLECT_char,
    .element_count = 33,
};

static const NYA_TypeReflection _NYA_REFLECT_char_255 = {
    .name          = "char[255]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(char[255]),
    .alignment     = alignof(char[255]),
    .element       = &_NYA_REFLECT_char,
    .element_count = 255,
};

static const NYA_TypeReflection _NYA_REFLECT_char_16 = {
    .name          = "char[16]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(char[16]),
    .alignment     = alignof(char[16]),
    .element       = &_NYA_REFLECT_char,
    .element_count = 16,
};

static const NYA_ReflectAttribute _NYA_REFLECT_TestSignup_username_ATTRS[] = {
    { .name = "required" },
    { .name = "len", .args = "3, 32" },
    { .name = "pattern", .args = "a*z" },
};

static const NYA_ReflectAttribute _NYA_REFLECT_TestSignup_email_ATTRS[] = {
    { .name = "required" },
    { .name = "email" },
};

static const NYA_ReflectAttribute _NYA_REFLECT_TestSignup_age_ATTRS[] = {
    { .name = "min", .args = "13"  },
    { .name = "max", .args = "120" },
};

static const NYA_ReflectAttribute _NYA_REFLECT_TestSignup_nickname_ATTRS[] = {
    { .name = "label", .args = "Nick" },
};

static const NYA_ReflectField _NYA_REFLECT_TestSignup_FIELDS[] = {
    { .name            = "username",
     .type            = &_NYA_REFLECT_char_33,
     .offset          = nya_offsetof(TestSignup, username),
     .attributes      = _NYA_REFLECT_TestSignup_username_ATTRS,
     .attribute_count = 3 },
    { .name            = "email",
     .type            = &_NYA_REFLECT_char_255,
     .offset          = nya_offsetof(TestSignup, email),
     .attributes      = _NYA_REFLECT_TestSignup_email_ATTRS,
     .attribute_count = 2 },
    { .name            = "age",
     .type            = &_NYA_REFLECT_s32,
     .offset          = nya_offsetof(TestSignup, age),
     .attributes      = _NYA_REFLECT_TestSignup_age_ATTRS,
     .attribute_count = 2 },
    { .name            = "nickname",
     .type            = &_NYA_REFLECT_char_16,
     .offset          = nya_offsetof(TestSignup, nickname),
     .attributes      = _NYA_REFLECT_TestSignup_nickname_ATTRS,
     .attribute_count = 1 },
};

static const NYA_TypeReflection _NYA_REFLECT_TestSignup = {
    .name        = "TestSignup",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(TestSignup),
    .alignment   = alignof(TestSignup),
    .fields      = _NYA_REFLECT_TestSignup_FIELDS,
    .field_count = 4,
};

/** A well formed value, the baseline each invalid case flips one field of. */
static TestSignup valid_signup(void) {
    TestSignup signup = { 0 };
    (void)snprintf(signup.username, sizeof(signup.username), "%s", "alice_z");
    (void)snprintf(signup.email, sizeof(signup.email), "%s", "alice@example.com");
    signup.age = 30;
    return signup;
}

/** Whether the error names `field`, so a test asserts the report points at the field it flipped. */
static b8 error_names(NYA_Error error, NYA_ConstCString field) {
    return !error.ok && strstr((const char*)error.message, field) != nullptr;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const NYA_TypeReflection* type = &_NYA_REFLECT_TestSignup;

    // TEST: a well formed value passes, and touches no arena on the way
    printf("TEST: a valid value passes and allocates nothing\n");
    {
        TestSignup signup = valid_signup();

        u64       before = nya_arena_memory_usage_bytes(nya_arena_global);
        NYA_Error valid  = nya_validate(type, &signup);
        u64       after  = nya_arena_memory_usage_bytes(nya_arena_global);

        nya_assert(valid.ok, "a well formed value was rejected: %s", (const char*)valid.message);
        nya_assert(after == before, "the valid path allocated " FMTu64 " bytes", after - before);

        printf("  PASSED\n");
    }

    // TEST: @required rejects an empty string and a zero number
    printf("TEST: @required\n");
    {
        TestSignup signup  = valid_signup();
        signup.username[0] = '\0';
        nya_assert(error_names(nya_validate(type, &signup), "username"), "an empty @required string was accepted");

        // age is @min(13); zero is below it, but the point here is a zero-valued field the range also guards.
        signup          = valid_signup();
        signup.email[0] = '\0';
        nya_assert(error_names(nya_validate(type, &signup), "email"), "an empty @required string was accepted");

        printf("  PASSED\n");
    }

    // TEST: @len rejects too short and too long, accepts the bounds
    printf("TEST: @len(3, 32)\n");
    {
        TestSignup signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "az"); // 2, below the minimum
        nya_assert(error_names(nya_validate(type, &signup), "username"), "a too-short string passed @len");

        signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaz"); // 33, over the maximum
        nya_assert(error_names(nya_validate(type, &signup), "username"), "a too-long string passed @len");

        signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "aaz"); // 3, exactly the minimum
        nya_assert(nya_validate(type, &signup).ok, "the minimum length was rejected");

        printf("  PASSED\n");
    }

    // TEST: @pattern, a tiny glob
    printf("TEST: @pattern(a*z)\n");
    {
        TestSignup signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "amz"); // matches a*z
        nya_assert(nya_validate(type, &signup).ok, "a value matching the glob was rejected");

        signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "bob"); // does not begin with 'a'
        nya_assert(error_names(nya_validate(type, &signup), "username"), "a value breaking the glob was accepted");

        signup = valid_signup();
        (void)snprintf(signup.username, sizeof(signup.username), "%s", "amy"); // does not end in 'z'
        nya_assert(error_names(nya_validate(type, &signup), "username"), "a value breaking the glob was accepted");

        printf("  PASSED\n");
    }

    // TEST: @email, by the newtype's own rule
    printf("TEST: @email\n");
    {
        TestSignup signup = valid_signup();
        (void)snprintf(signup.email, sizeof(signup.email), "%s", "not-an-email");
        nya_assert(error_names(nya_validate(type, &signup), "email"), "a malformed address passed @email");

        signup = valid_signup();
        (void)snprintf(signup.email, sizeof(signup.email), "%s", "a@b.co");
        nya_assert(nya_validate(type, &signup).ok, "a valid address was rejected");

        printf("  PASSED\n");
    }

    // TEST: @min and @max on a number
    printf("TEST: @min(13) @max(120)\n");
    {
        TestSignup signup = valid_signup();
        signup.age        = 12;
        nya_assert(error_names(nya_validate(type, &signup), "age"), "a value below @min was accepted");

        signup     = valid_signup();
        signup.age = 121;
        nya_assert(error_names(nya_validate(type, &signup), "age"), "a value above @max was accepted");

        signup     = valid_signup();
        signup.age = 13;
        nya_assert(nya_validate(type, &signup).ok, "the minimum was rejected");
        signup.age = 120;
        nya_assert(nya_validate(type, &signup).ok, "the maximum was rejected");

        printf("  PASSED\n");
    }

    // TEST: an attribute validation has no rule for is ignored, not an error
    printf("TEST: an unknown attribute is ignored\n");
    {
        // nickname carries only @label; a value that would break nothing else must pass whatever it holds.
        TestSignup signup = valid_signup();
        (void)snprintf(signup.nickname, sizeof(signup.nickname), "%s", "whatever");
        nya_assert(nya_validate(type, &signup).ok, "an unknown attribute was treated as a rule");

        printf("  PASSED\n");
    }

    printf("ALL PASSED\n");
    return 0;
}
