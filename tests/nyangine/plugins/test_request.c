/**
 * The curl plugin: argument validation, error mapping, and the one transport failure that can be
 * produced without a network.
 *
 * **Nothing here reaches the internet.** A test that resolved a hostname would fail in CI for
 * reasons having nothing to do with this code, and would fail differently depending on whose
 * machine it ran on. What is left is still most of the module: every early return, the method
 * table, the status classifier, and a connection refused against a closed port on loopback — which
 * needs no DNS, no route and no server, and always fails immediately.
 *
 * What is deliberately *not* covered: a successful 2xx round trip, and therefore the JSON parsing
 * of a response body. That needs a server. The parsing itself is serde_json's, which test_serde
 * covers; what is untested here is the wiring between them.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/**
 * A port on loopback that nothing is listening on.
 *
 * Port 1 is reserved and never bound by anything on a normal machine, and connecting to loopback
 * needs neither DNS nor a route, so this fails with "connection refused" in microseconds on every
 * platform rather than hanging for a timeout.
 */
#define CLOSED_PORT_URL "http://127.0.0.1:1/"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_request");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the method table matches what goes on the wire
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_string_equals((NYA_CString)nya_request_method_name(NYA_REQUEST_METHOD_GET), "GET"));
    nya_assert(nya_string_equals((NYA_CString)nya_request_method_name(NYA_REQUEST_METHOD_POST), "POST"));
    nya_assert(nya_string_equals((NYA_CString)nya_request_method_name(NYA_REQUEST_METHOD_PUT), "PUT"));
    nya_assert(nya_string_equals((NYA_CString)nya_request_method_name(NYA_REQUEST_METHOD_PATCH), "PATCH"));
    nya_assert(nya_string_equals((NYA_CString)nya_request_method_name(NYA_REQUEST_METHOD_DELETE), "DELETE"));

    // The sentinel is not a method, and neither is anything past it.
    nya_assert(nya_request_method_name(NYA_REQUEST_METHOD_COUNT) == nullptr);
    nya_assert(nya_request_method_name((NYA_RequestMethod)999) == nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the success classifier is exactly 2xx
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Exposed so a caller that tolerates a 404 asks the question the same way the module does,
    // rather than open coding a range that drifts from it.
    nya_assert(nya_request_status_is_success(200));
    nya_assert(nya_request_status_is_success(201));
    nya_assert(nya_request_status_is_success(204));
    nya_assert(nya_request_status_is_success(299));

    nya_assert(!nya_request_status_is_success(199), "1xx is not success");
    nya_assert(!nya_request_status_is_success(300), "nor is a redirect");
    nya_assert(!nya_request_status_is_success(404));
    nya_assert(!nya_request_status_is_success(500));
    nya_assert(!nya_request_status_is_success(0), "zero means no response arrived at all");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a malformed request is rejected before anything is attempted
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Response response = { 0 };

    NYA_Error no_url = nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = nullptr }, &response);
    nya_assert(no_url.kind == NYA_ERROR_INVALID_ARGUMENT, "a null url is a caller mistake");

    NYA_Error empty_url = nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = "" }, &response);
    nya_assert(empty_url.kind == NYA_ERROR_INVALID_ARGUMENT, "and so is an empty one");

    NYA_Error bad_method = nya_request_perform(
      arena, (NYA_Request){ .method = (NYA_RequestMethod)999, .url = "http://127.0.0.1/" }, &response
    );
    nya_assert(bad_method.kind == NYA_ERROR_INVALID_ARGUMENT, "an unknown method too");

    // Left untouched on these paths on purpose: there was never a response to describe, and zeroing
    // it would be indistinguishable from a real transfer that returned nothing.
    nya_assert(response.status == 0);
    nya_assert(response.raw_body == nullptr, "nothing was allocated for a request that never ran");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a url with no scheme, and one with a scheme curl is not allowed to use
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Response response = { 0 };

    // The protocol allowlist is a security control, not a tidiness one: without it a url from a
    // config file or a redirect can reach file:// and turn "fetch the leaderboard" into a local
    // file read. Both of these must fail rather than succeed at reading something.
    NYA_Error file_scheme = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = "file:///etc/passwd", .timeout_ms = 2000 }, &response
    );
    nya_assert(!file_scheme.ok, "file:// is not an allowed protocol");
    nya_assert(response.status == 0, "and nothing was fetched");

    NYA_Error ftp_scheme = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = "ftp://127.0.0.1/x", .timeout_ms = 2000 }, &response
    );
    nya_assert(!ftp_scheme.ok, "neither is ftp://");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a refused connection is an error, and the response is still initialised
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Response response = { 0 };

    NYA_Error result = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = CLOSED_PORT_URL, .timeout_ms = 3000 }, &response
    );

    nya_assert(!result.ok, "nothing is listening, so this cannot have succeeded");

    // Mapped rather than passed through as a generic failure: a caller deciding whether to retry
    // wants to know the difference between "no route" and "the server said no".
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "a refused connection maps to NOT_FOUND, got %d", (int)result.kind);

    // Past the argument checks, so the response *is* set up even though the transfer failed. Status
    // stays zero because no response was ever received.
    nya_assert(response.status == 0, "no HTTP response means no status, got %u", response.status);
    nya_assert(response.raw_body != nullptr, "the body buffer exists even when nothing arrived");
    nya_assert(response.raw_body->length == 0, "and it is empty");
    nya_assert(response.body == nullptr, "nothing to parse");
    nya_assert(response.content_type == nullptr, "and no headers came back");

    // The message names the method and the url, which is what makes a log line actionable.
    NYA_ConstCString message = (NYA_ConstCString)result.message;
    nya_assert(nya_string_contains(message, "GET"), "got '%s'", message);
    nya_assert(nya_string_contains(message, "127.0.0.1"), "got '%s'", message);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: every method reaches the transport, not just GET
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A write method with no body used to be the case that hung: curl waits for a body it was told
    // to expect and the peer times out. Against a closed port each of these has to fail the same
    // fast way GET does, which is what proves the request was fully formed before it was sent.
    NYA_RequestMethod methods[] = {
      NYA_REQUEST_METHOD_GET, NYA_REQUEST_METHOD_POST, NYA_REQUEST_METHOD_PUT, NYA_REQUEST_METHOD_PATCH, NYA_REQUEST_METHOD_DELETE,
    };

    for (u32 i = 0; i < nya_carray_length(methods); i++) {
      NYA_Response response = { 0 };
      NYA_Error    result   = nya_request_perform(arena, (NYA_Request){ .method = methods[i], .url = CLOSED_PORT_URL, .timeout_ms = 3000 }, &response);

      nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "%s did not fail the way GET does, got %d", nya_request_method_name(methods[i]), (int)result.kind);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a JSON body is serialized and does not change the failure mode
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object* body = nya_object_create(arena);
    nya_object_set(body, "score", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 4200 });
    nya_object_set(body, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"player" });

    NYA_Response response = { 0 };
    NYA_Error    result   = nya_request_perform(
      arena,
      (NYA_Request){
        .method       = NYA_REQUEST_METHOD_POST,
        .url          = CLOSED_PORT_URL,
        .body         = body,
        .bearer_token = "a-token",
        .timeout_ms   = 3000,
        .headers      = { { .name = "X-Test", .value = "1" } },
      },
      &response
    );

    // Serializing the body, building the header list and setting auth all happen before the
    // transfer; if any of them faulted this would not reach the transport at all.
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "reached the transport with a body attached, got %d", (int)result.kind);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a GET ignores a body rather than refusing it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Not an error, deliberately: a shared request struct filled in by a helper should not become a
    // special case at every call site just because this one is a GET.
    NYA_Object* body = nya_object_create(arena);
    nya_object_set(body, "ignored", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });

    NYA_Response response = { 0 };
    NYA_Error    result   = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = CLOSED_PORT_URL, .body = body, .timeout_ms = 3000 }, &response
    );

    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "a GET with a body is still just a GET, got %d", (int)result.kind);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the convenience wrappers behave like the full call
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Response response = { 0 };

    NYA_Error get = nya_request_get(arena, CLOSED_PORT_URL, &response);
    nya_assert(get.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(nya_string_contains((NYA_ConstCString)get.message, "GET"), "the wrapper really did send a GET");

    NYA_Object* body = nya_object_create(arena);
    nya_object_set(body, "x", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });

    NYA_Error post = nya_request_post(arena, CLOSED_PORT_URL, body, &response);
    nya_assert(post.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(nya_string_contains((NYA_ConstCString)post.message, "POST"), "and this one a POST");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a timeout that cannot be met is reported as a timeout
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // 1ms against a documentation address that is routed nowhere. TEST-NET-1 (192.0.2.0/24) is
    // reserved by RFC 5737 precisely so it never reaches a real host, so this cannot accidentally
    // contact anything — it either times out or fails to route, and both are acceptable answers.
    NYA_Response response = { 0 };
    NYA_Error    result   = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = "http://192.0.2.1/", .timeout_ms = 1 }, &response
    );

    nya_assert(!result.ok, "an unroutable address cannot succeed");
    nya_assert(
      result.kind == NYA_ERROR_TIMEOUT || result.kind == NYA_ERROR_NOT_FOUND || result.kind == NYA_ERROR_IO,
      "expected a transport failure, got %d",
      (int)result.kind
    );
  }

  printf("PASSED: test_request\n");
  return 0;
}
