/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

typedef struct {
  NYA_CString name;
  u8          age;
} TestUser;
nya_derive_maybe(TestUser);

NYA_Error always_ok(void) {
  return NYA_OK;
}

NYA_Error always_fail(void) {
  return nya_error(NYA_ERROR_NOT_OK);
}

NYA_Error always_fail_msg(void) {
  return nya_error(NYA_ERROR_NOT_OK, "something broke");
}

NYA_Error always_fail_fmt(void) {
  return nya_error(NYA_ERROR_NOT_OK, "code %d", 42);
}

NYA_Error try_ok(void) {
  NYA_TRY(always_ok());
  return NYA_OK;
}

NYA_Error try_fail(void) {
  NYA_TRY(always_fail());
  return NYA_OK;
}

NYA_Error always_fail_not_found(void) {
  return nya_error(NYA_ERROR_NOT_FOUND, "file missing: %s", "/tmp/gone");
}

NYA_Error try_fail_not_found(void) {
  NYA_TRY(always_fail_not_found());
  return NYA_OK;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_OK - returns a result with no error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = NYA_OK;
    nya_assert(result.error == NYA_ERROR_NONE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_Error enum - values are correct
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(NYA_ERROR_NONE == 0);
  nya_assert(NYA_ERROR_NOT_OK == 1);
  nya_assert(NYA_ERROR_NOT_FOUND == 2);
  nya_assert(NYA_ERROR_PERMISSION_DENIED == 3);
  nya_assert(NYA_ERROR_OUT_OF_MEMORY == 4);
  nya_assert(NYA_ERROR_PARSE_ERROR == 5);
  nya_assert(NYA_ERROR_COUNT == 6);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_ERRORKIND_NAME_MAP - maps error codes to strings
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NONE], "NONE") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NOT_OK], "GENERIC") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NOT_FOUND], "NOT_FOUND") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_PERMISSION_DENIED], "PERMISSION_DENIED") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_OUT_OF_MEMORY], "OUT_OF_MEMORY") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_PARSE_ERROR], "PARSE_ERROR") == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code only
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK);
    nya_assert(result.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code and message
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK, "test message");
    nya_assert(result.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code, format, and arguments
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK, "error %d: %s", 42, "fail");
    nya_assert(result.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_result - panics on null format string
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert_panic((void)_nya_result(NYA_ERROR_NOT_OK, nullptr));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_result - panics on invalid error code
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert_panic((void)_nya_result(NYA_ERROR_COUNT, ""));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - passes through on success
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_ok();
    nya_assert(result.error == NYA_ERROR_NONE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - propagates error on failure
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_fail();
    nya_assert(result.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_EXPECT - passes on success
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(always_ok());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_EXPECT - panics on failure
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert_panic(NYA_EXPECT(always_fail()));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_derive_maybe / nya_none - has_value is false
  // ─────────────────────────────────────────────────────────────────────────────
  {
    MaybeTestUser maybe = nya_none(TestUser);
    nya_assert(maybe.has_value == false);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_derive_maybe / nya_some - has_value is true and value is correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    TestUser user = { .name = "Alice", .age = 25 };
    MaybeTestUser maybe = nya_some(TestUser, user);
    nya_assert(maybe.has_value == true);
    nya_assert(strcmp(maybe.value.name, "Alice") == 0);
    nya_assert(maybe.value.age == 25);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_some - inline struct initialization
  // ─────────────────────────────────────────────────────────────────────────────
  {
    MaybeTestUser maybe = nya_some(TestUser, ((TestUser){ .name = "Bob", .age = 30 }));
    nya_assert(maybe.has_value == true);
    nya_assert(strcmp(maybe.value.name, "Bob") == 0);
    nya_assert(maybe.value.age == 30);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Function returning result - success path
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = always_ok();
    nya_assert(result.error == NYA_ERROR_NONE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Function returning result - failure paths
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r1 = always_fail();
    nya_assert(r1.error == NYA_ERROR_NOT_OK);

    NYA_Error r2 = always_fail_msg();
    nya_assert(r2.error == NYA_ERROR_NOT_OK);

    NYA_Error r3 = always_fail_fmt();
    nya_assert(r3.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - captures errno into result
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = ENOENT;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.error == NYA_ERROR_NOT_FOUND);
    nya_assert(strstr(result.message, strerror(ENOENT)) != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - maps errno to correct error codes
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // NOT_FOUND
    errno = ENOENT;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);
    errno = ESRCH;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);
    errno = ENODEV;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);
    errno = ENOTDIR;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);

    // PERMISSION_DENIED
    errno = EACCES;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_PERMISSION_DENIED);
    errno = EPERM;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_PERMISSION_DENIED);
    errno = EROFS;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_PERMISSION_DENIED);

    // OUT_OF_MEMORY
    errno = ENOMEM;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_OUT_OF_MEMORY);

    // GENERIC (default fallback)
    errno = EEXIST;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = EIO;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = EINVAL;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = ENOTSUP;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = ETIMEDOUT;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - unknown errno falls back to GENERIC
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = 9999;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - message contains errno number
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = EINVAL;
    NYA_Error result = nya_error_from_errno();
    char expected_suffix[32];
    snprintf(expected_suffix, sizeof(expected_suffix), "errno %d", EINVAL);
    nya_assert(strstr(result.message, expected_suffix) != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - message content is correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r1 = nya_error(NYA_ERROR_NOT_OK, "test message");
    nya_assert(strcmp(r1.message, "test message") == 0);

    NYA_Error r2 = nya_error(NYA_ERROR_NOT_FOUND, "disk %d failed at sector %d", 3, 42);
    nya_assert(strcmp(r2.message, "disk 3 failed at sector 42") == 0);

    NYA_Error r3 = nya_error(NYA_ERROR_NOT_FOUND);
    nya_assert(strcmp(r3.message, "") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - all error codes produce correct results
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r;
    r = nya_error(NYA_ERROR_NOT_FOUND, "gone");
    nya_assert(r.error == NYA_ERROR_NOT_FOUND);

    r = nya_error(NYA_ERROR_PERMISSION_DENIED, "no");
    nya_assert(r.error == NYA_ERROR_PERMISSION_DENIED);

    r = nya_error(NYA_ERROR_OUT_OF_MEMORY, "oom");
    nya_assert(r.error == NYA_ERROR_OUT_OF_MEMORY);

    r = nya_error(NYA_ERROR_PARSE_ERROR, "syntax");
    nya_assert(r.error == NYA_ERROR_PARSE_ERROR);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - preserves specific error codes (not just GENERIC)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_fail_not_found();
    nya_assert(result.error == NYA_ERROR_NOT_FOUND);
    nya_assert(strstr(result.message, "file missing") != nullptr);
    nya_assert(strstr(result.message, "/tmp/gone") != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_OK - message is empty
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = NYA_OK;
    nya_assert(result.error == NYA_ERROR_NONE);
    nya_assert(result.message[0] == '\0');
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - long message is truncated, not overflowed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char long_msg[1024];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    NYA_Error result = nya_error(NYA_ERROR_NOT_OK, "%s", long_msg);
    nya_assert(result.error == NYA_ERROR_NOT_OK);
    nya_assert(strlen(result.message) < 512);
    nya_assert(strlen(result.message) == 511);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - additional errno mappings
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // NOT_FOUND extras
    errno = ENXIO;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);
    errno = ENAMETOOLONG;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_FOUND);

    // GENERIC fallback extras
    errno = ESPIPE;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = EMFILE;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = ENFILE;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = EISDIR;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = ENOTTY;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);

    // GENERIC fallback extras
    errno = E2BIG;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
    errno = EFAULT;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);

    // GENERIC fallback extras
    errno = ENOSYS;
    nya_assert(nya_error_from_errno().error == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - message contains strerror text
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = EACCES;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.error == NYA_ERROR_PERMISSION_DENIED);
    nya_assert(strstr(result.message, strerror(EACCES)) != nullptr);

    errno = ENOMEM;
    result = nya_error_from_errno();
    nya_assert(result.error == NYA_ERROR_OUT_OF_MEMORY);
    nya_assert(strstr(result.message, strerror(ENOMEM)) != nullptr);
  }

  return 0;
}
