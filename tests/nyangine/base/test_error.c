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
    nya_assert(result.ok);
    nya_assert(result.ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: `ok` agrees with `kind` everywhere an NYA_Error can be born
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * `ok` is redundant state, so the only thing that keeps it true is that every NYA_Error comes
     * out of one of three places. This asserts the invariant at all three rather than trusting it:
     * a fourth construction site added later — or a designated initializer that names `kind` and
     * forgets this — fails here rather than silently reporting every error as a success.
     */
    nya_assert(NYA_OK.ok, "NYA_OK must be ok");
    nya_assert(NYA_OK.ok);

    nya_assert(!NYA_NOT_OK.ok, "NYA_NOT_OK must not be ok");
    nya_assert(NYA_NOT_OK.kind == NYA_ERROR_NOT_OK);

    // _nya_error_create, across every kind there is, including NONE — which is legal to construct
    // and is the one case where the derivation is not simply "false".
    for (u32 kind = 0; kind < NYA_ERROR_COUNT; kind++) {
      NYA_Error error = nya_error((NYA_ErrorKind)kind, "kind %u", kind);

      nya_assert(error.kind == (NYA_ErrorKind)kind);
      nya_assert(error.ok == (kind == NYA_ERROR_NONE), "ok disagreed with kind %s", NYA_ERRORKIND_NAME_MAP[kind]);
    }

    // And through errno, which builds its result with _nya_error_create rather than by hand.
    errno            = ENOENT;
    NYA_Error from_errno = nya_error_from_errno();
    nya_assert(from_errno.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(!from_errno.ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: `ok` survives propagation through NYA_TRY
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // NYA_TRY copies the struct into the caller's frame and pushes a trace frame onto it, so this is
    // checking that the flag rides along with the kind rather than being recomputed on the way.
    NYA_Error ok_result = try_ok();
    nya_assert(ok_result.ok);
    nya_assert(ok_result.ok);

    NYA_Error fail_result = try_fail();
    nya_assert(!fail_result.ok);
    nya_assert(fail_result.kind == NYA_ERROR_NOT_OK);
    nya_assert(fail_result.error_trace_count > 0, "the propagation frame was still recorded");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_Error enum - values are correct
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(NYA_ERROR_NONE == 0);
  nya_assert(NYA_ERROR_NOT_OK == 1);
  nya_assert(NYA_ERROR_NOT_FOUND == 2);
  nya_assert(NYA_ERROR_PERMISSION_DENIED == 3);
  nya_assert(NYA_ERROR_ALREADY_EXISTS == 4);
  nya_assert(NYA_ERROR_INVALID_ARGUMENT == 5);
  nya_assert(NYA_ERROR_OUT_OF_MEMORY == 6);
  nya_assert(NYA_ERROR_IO == 7);
  nya_assert(NYA_ERROR_TIMEOUT == 8);
  nya_assert(NYA_ERROR_NOT_SUPPORTED == 9);
  nya_assert(NYA_ERROR_CORRUPT == 10);
  nya_assert(NYA_ERROR_PARSE == 11);
  nya_assert(NYA_ERROR_COUNT == 12);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_ERRORKIND_NAME_MAP - every kind is named, and named correctly
  //
  // The loop is the part that matters: a kind added without a map entry leaves a null there, and
  // the first thing to notice would otherwise be a crash while formatting some unrelated error.
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 kind = 0; kind < NYA_ERROR_COUNT; kind++) { nya_assert(NYA_ERRORKIND_NAME_MAP[kind] != nullptr); }

  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NONE], "NONE") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NOT_OK], "NOT_OK") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NOT_FOUND], "NOT_FOUND") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_PERMISSION_DENIED], "PERMISSION_DENIED") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_ALREADY_EXISTS], "ALREADY_EXISTS") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_INVALID_ARGUMENT], "INVALID_ARGUMENT") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_OUT_OF_MEMORY], "OUT_OF_MEMORY") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_IO], "IO") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_TIMEOUT], "TIMEOUT") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_NOT_SUPPORTED], "NOT_SUPPORTED") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_CORRUPT], "CORRUPT") == 0);
  nya_assert(strcmp(NYA_ERRORKIND_NAME_MAP[NYA_ERROR_PARSE], "PARSE") == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code only
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK);
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code and message
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK, "test message");
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - with error code, format, and arguments
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = nya_error(NYA_ERROR_NOT_OK, "error %d: %s", 42, "fail");
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_error_create - panics on null format string
  //
  // Called directly rather than through _nya_error: with two arguments that macro selects
  // _NYA_ERROR2, which passes "%s" as the format and the caller's value as the message, so a null
  // there is a null *message* and never reaches the format assertion.
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash((void)_nya_error_create(NYA_ERROR_NOT_OK, nullptr));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_error - panics on invalid error code
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash((void)_nya_error(NYA_ERROR_COUNT, ""));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - passes through on success
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_ok();
    nya_assert(result.ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - propagates error on failure
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_fail();
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_EXPECT - passes on success
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(always_ok());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_EXPECT - panics on failure
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(NYA_EXPECT(always_fail()));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_derive_maybe / nya_none - has_value is false
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_MaybeᐸTestUserᐳ maybe = nya_none(TestUser);
    nya_assert(maybe.has_value == false);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_derive_maybe / nya_some - has_value is true and value is correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    TestUser user = { .name = "Alice", .age = 25 };
    NYA_MaybeᐸTestUserᐳ maybe = nya_some(TestUser, user);
    nya_assert(maybe.has_value == true);
    nya_assert(strcmp(maybe.value.name, "Alice") == 0);
    nya_assert(maybe.value.age == 25);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_some - inline struct initialization
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_MaybeᐸTestUserᐳ maybe = nya_some(TestUser, ((TestUser){ .name = "Bob", .age = 30 }));
    nya_assert(maybe.has_value == true);
    nya_assert(strcmp(maybe.value.name, "Bob") == 0);
    nya_assert(maybe.value.age == 30);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Function returning result - success path
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = always_ok();
    nya_assert(result.ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Function returning result - failure paths
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r1 = always_fail();
    nya_assert(r1.kind == NYA_ERROR_NOT_OK);

    NYA_Error r2 = always_fail_msg();
    nya_assert(r2.kind == NYA_ERROR_NOT_OK);

    NYA_Error r3 = always_fail_fmt();
    nya_assert(r3.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - captures errno into result
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = ENOENT;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(strstr((const char*)result.message, strerror(ENOENT)) != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - maps errno to correct error codes
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // NOT_FOUND
    errno = ENOENT;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_FOUND);
    errno = ESRCH;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_FOUND);
    errno = ENODEV;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_FOUND);
    errno = ENOTDIR;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_FOUND);

    // PERMISSION_DENIED
    errno = EACCES;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_PERMISSION_DENIED);
    errno = EPERM;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_PERMISSION_DENIED);

    // These used to be folded into the generic kind. The mapping now distinguishes them, which is
    // the whole reason a caller can tell "already there" from "no permission" without reading text.
    errno = EEXIST;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_ALREADY_EXISTS);
    errno = EINVAL;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_INVALID_ARGUMENT);
    errno = ENAMETOOLONG;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_INVALID_ARGUMENT);
    errno = ENOMEM;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_OUT_OF_MEMORY);
    errno = EIO;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_IO);
    errno = ETIMEDOUT;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_TIMEOUT);
    errno = ENOSYS;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_SUPPORTED);
    errno = ENOTSUP;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_SUPPORTED);

    // Not in the mapping, so it lands on the generic kind. Arguably it should be
    // PERMISSION_DENIED — a read only filesystem is a permission problem — but that is the
    // engine's call to make, and this pins what it does today rather than what it might.
    errno = EROFS;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - unknown errno falls back to GENERIC
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = 9999;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - message contains errno number
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = EINVAL;
    NYA_Error result = nya_error_from_errno();
    char expected_suffix[32];
    snprintf(expected_suffix, sizeof(expected_suffix), "errno %d", EINVAL);
    nya_assert(strstr((const char*)result.message, expected_suffix) != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - message content is correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r1 = nya_error(NYA_ERROR_NOT_OK, "test message");
    nya_assert(strcmp((const char*)r1.message, "test message") == 0);

    NYA_Error r2 = nya_error(NYA_ERROR_NOT_FOUND, "disk %d failed at sector %d", 3, 42);
    nya_assert(strcmp((const char*)r2.message, "disk 3 failed at sector 42") == 0);

    NYA_Error r3 = nya_error(NYA_ERROR_NOT_FOUND);
    nya_assert(strcmp((const char*)r3.message, "") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_err - all error codes produce correct results
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r;
    r = nya_error(NYA_ERROR_NOT_FOUND, "gone");
    nya_assert(r.kind == NYA_ERROR_NOT_FOUND);

    r = nya_error(NYA_ERROR_PERMISSION_DENIED, "no");
    nya_assert(r.kind == NYA_ERROR_PERMISSION_DENIED);

    r = nya_error(NYA_ERROR_OUT_OF_MEMORY, "oom");
    nya_assert(r.kind == NYA_ERROR_OUT_OF_MEMORY);

    r = nya_error(NYA_ERROR_PARSE, "syntax");
    nya_assert(r.kind == NYA_ERROR_PARSE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_TRY - preserves specific error codes (not just GENERIC)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = try_fail_not_found();
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(strstr((const char*)result.message, "file missing") != nullptr);
    nya_assert(strstr((const char*)result.message, "/tmp/gone") != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_OK - message is empty
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error result = NYA_OK;
    nya_assert(result.ok);
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
    nya_assert(result.kind == NYA_ERROR_NOT_OK);
    // Written against the macro rather than a literal: the buffer was 512 when this was first
    // written and is 192 now, and a hardcoded length just moves the breakage to the next change.
    nya_assert(strlen((const char*)result.message) == NYA_ERROR_MESSAGE_MAX_LENGTH - 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - additional errno mappings
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // NOT_FOUND extras
    errno = ENXIO;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_FOUND);
    // ENAMETOOLONG moved: it is a bad argument, not a missing file.
    errno = ENAMETOOLONG;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_INVALID_ARGUMENT);

    // GENERIC fallback extras
    errno = ESPIPE;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
    errno = EMFILE;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
    errno = ENFILE;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
    errno = EISDIR;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
    errno = ENOTTY;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);

    // GENERIC fallback extras
    errno = E2BIG;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);
    errno = EFAULT;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_OK);

    // ENOSYS moved too: an unimplemented syscall is NOT_SUPPORTED, not a generic failure.
    errno = ENOSYS;
    nya_assert(nya_error_from_errno().kind == NYA_ERROR_NOT_SUPPORTED);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_error_from_errno - message contains strerror text
  // ─────────────────────────────────────────────────────────────────────────────
  {
    errno = EACCES;
    NYA_Error result = nya_error_from_errno();
    nya_assert(result.kind == NYA_ERROR_PERMISSION_DENIED);
    nya_assert(strstr((const char*)result.message, strerror(EACCES)) != nullptr);

    errno = ENOMEM;
    result = nya_error_from_errno();
    nya_assert(result.kind == NYA_ERROR_OUT_OF_MEMORY);
    nya_assert(strstr((const char*)result.message, strerror(ENOMEM)) != nullptr);
  }

  return 0;
}
