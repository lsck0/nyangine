/**
 * The curl plugin: argument validation, error mapping, and the one transport failure that can be
 * produced without a network.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/**
 * A port on loopback that nothing is listening on.
 */
#define CLOSED_PORT_URL "http://127.0.0.1:1/"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_request");
  defer      nya_arena_destroy(arena);

  // TEST: the method table matches what goes on the wire
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

  // TEST: the success classifier is exactly 2xx
  {
    // Exposed so a caller that tolerates a 404 asks the question the same way the module does, rather than open coding a range that drifts from it.
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

  // TEST: a malformed request is rejected before anything is attempted
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

    // Left untouched on these paths on purpose: there was never a response to describe, and zeroing it would be indistinguishable from a real transfer that returned nothing.
    nya_assert(response.status == 0);
    nya_assert(response.raw_body == nullptr, "nothing was allocated for a request that never ran");
  }

  // TEST: a url with no scheme, and one with a scheme curl is not allowed to use
  {
    NYA_Response response = { 0 };

    // The protocol allowlist is a security control, not a tidiness one: without it a url from a config file or a redirect can reach file:// and turn "fetch the leaderboard" into a local file read. Both of these must fail rather than succeed at reading something.
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

  // TEST: a refused connection is an error, and the response is still initialised
  {
    NYA_Response response = { 0 };

    NYA_Error result = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = CLOSED_PORT_URL, .timeout_ms = 3000 }, &response
    );

    nya_assert(!result.ok, "nothing is listening, so this cannot have succeeded");

    // Mapped rather than passed through as a generic failure: a caller deciding whether to retry wants to know the difference between "no route" and "the server said no".
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "a refused connection maps to NOT_FOUND, got %d", (int)result.kind);

    // Past the argument checks, so the response *is* set up even though the transfer failed. Status stays zero because no response was ever received.
    nya_assert(response.status == 0, "no HTTP response means no status, got %u", response.status);
    nya_assert(response.raw_body != nullptr, "the body buffer exists even when nothing arrived");
    nya_assert(response.raw_body->length == 0, "and it is empty");
    nya_assert(response.body == nullptr, "nothing to parse");
    nya_assert(response.content_type == nullptr, "and no headers came back");
    nya_assert(response.raw_headers != nullptr && response.raw_headers->length == 0, "the header buffer exists and is empty");

    // The message names the method and the url, which is what makes a log line actionable.
    NYA_ConstCString message = (NYA_ConstCString)result.message;
    nya_assert(nya_string_contains(message, "GET"), "got '%s'", message);
    nya_assert(nya_string_contains(message, "127.0.0.1"), "got '%s'", message);
  }

  // TEST: every method reaches the transport, not just GET
  {
    // a write method with no body must not hang with curl waiting for a body. Against a closed port each must fail as fast as GET, proving the request was fully formed.
    NYA_RequestMethod methods[] = {
      NYA_REQUEST_METHOD_GET, NYA_REQUEST_METHOD_POST, NYA_REQUEST_METHOD_PUT, NYA_REQUEST_METHOD_PATCH, NYA_REQUEST_METHOD_DELETE,
    };

    for (u32 i = 0; i < nya_carray_length(methods); i++) {
      NYA_Response response = { 0 };
      NYA_Error    result   = nya_request_perform(arena, (NYA_Request){ .method = methods[i], .url = CLOSED_PORT_URL, .timeout_ms = 3000 }, &response);

      nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "%s did not fail the way GET does, got %d", nya_request_method_name(methods[i]), (int)result.kind);
    }
  }

  // TEST: a JSON body is serialized and does not change the failure mode
  {
    NYA_Object* body = nya_object_create(arena);
    nya_object_add(body, "score", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 4200 });
    nya_object_add(body, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"player" });

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

    // Serializing the body, building the header list and setting auth all happen before the transfer; if any of them faulted this would not reach the transport at all.
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "reached the transport with a body attached, got %d", (int)result.kind);
  }

  // TEST: a GET ignores a body rather than refusing it
  {
    // Not an error, deliberately: a shared request struct filled in by a helper should not become a special case at every call site just because this one is a GET.
    NYA_Object* body = nya_object_create(arena);
    nya_object_add(body, "ignored", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });

    NYA_Response response = { 0 };
    NYA_Error    result   = nya_request_perform(
      arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = CLOSED_PORT_URL, .body = body, .timeout_ms = 3000 }, &response
    );

    nya_assert(result.kind == NYA_ERROR_NOT_FOUND, "a GET with a body is still just a GET, got %d", (int)result.kind);
  }

  // TEST: a response header is found by name, whatever case the server sent it in
  {
    // Built by hand rather than fetched: what is in question is the reading, and a real server would decide the casing and the spacing for us.
    NYA_Response response = {
      .status      = 200,
      .raw_body    = nya_string_create(arena),
      .raw_headers = nya_string_from(arena, "Content-Type: application/json\r\nRetry-After:  1.5\t\nx-empty:\n"),
    };

    char value[64] = { 0 };

    nya_assert(nya_response_header(&response, "content-type", value, sizeof(value)));
    nya_assert(nya_string_equals(value, "application/json"), "matched case insensitively, as HTTP requires");

    nya_assert(nya_response_header(&response, "RETRY-AFTER", value, sizeof(value)));
    nya_assert(nya_string_equals(value, "1.5"), "with the padding and the line ending off, got '%s'", value);

    nya_assert(nya_response_header(&response, "x-empty", value, sizeof(value)), "a header with an empty value is still there");
    nya_assert(nya_string_equals(value, ""));

    nya_assert(!nya_response_header(&response, "x-absent", value, sizeof(value)));
    nya_assert(nya_string_equals(value, ""), "and the buffer is cleared rather than left holding the last answer");

    // Refused rather than truncated: callers turn these into numbers, and half of a number is a wrong answer where a missing one is a known unknown.
    char tiny[4] = { 0 };
    nya_assert(!nya_response_header(&response, "content-type", tiny, sizeof(tiny)));

    // A name that is a prefix of a real one must not match it.
    nya_assert(!nya_response_header(&response, "content", value, sizeof(value)));
  }

  // TEST: the convenience wrappers behave like the full call
  {
    NYA_Response response = { 0 };

    NYA_Error get = nya_request_get(arena, CLOSED_PORT_URL, &response);
    nya_assert(get.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(nya_string_contains((NYA_ConstCString)get.message, "GET"), "the wrapper really did send a GET");

    NYA_Object* body = nya_object_create(arena);
    nya_object_add(body, "x", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });

    NYA_Error post = nya_request_post(arena, CLOSED_PORT_URL, body, &response);
    nya_assert(post.kind == NYA_ERROR_NOT_FOUND);
    nya_assert(nya_string_contains((NYA_ConstCString)post.message, "POST"), "and this one a POST");
  }

  // TEST: a timeout that cannot be met is reported as a timeout
  {
    // 1ms against TEST-NET-1 (192.0.2.0/24), reserved by RFC 5737 to never reach a real host. Timing out and failing to route are both acceptable.
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

  // TEST: a form encoded body, which is what a token endpoint takes.
  {
    NYA_Object* body = nya_object_create(arena);

    nya_object_add(body, "grant_type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"authorization_code" });

    // the characters that make this worth encoding at all: a code_verifier is base64url and a redirect uri is a url, and `: / = + &` in an unencoded body would read as structure.
    nya_object_add(body, "redirect_uri", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"https://app.test/callback?x=1" });
    nya_object_add(body, "code", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"a+b/c=d&e" });
    nya_object_add(body, "expires_in", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 3600 });
    nya_object_add(body, "consent", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    NYA_String* encoded = nullptr;
    nya_assert(_nya_request_form_encode(arena, body, &encoded).ok, "a flat object encodes");

    NYA_ConstCString text = nya_string_to_cstring(arena, encoded);

    nya_assert(nya_string_contains(text, "grant_type=authorization_code"), "a plain value stands for itself, got '%s'", text);
    nya_assert(nya_string_contains(text, "code=a%2Bb%2Fc%3Dd%26e"), "and every reserved character is escaped, got '%s'", text);
    nya_assert(nya_string_contains(text, "redirect_uri=https%3A%2F%2Fapp.test%2Fcallback%3Fx%3D1"), "including in a url, got '%s'", text);
    nya_assert(nya_string_contains(text, "expires_in=3600"), "a number is written as one, got '%s'", text);
    nya_assert(nya_string_contains(text, "consent=true"), "and so is a boolean, got '%s'", text);
    nya_assert(!nya_string_contains(text, "&&") && text[0] != '&', "pairs are joined by one separator, got '%s'", text);

    // a form body has nowhere to put nesting, so this is an error rather than something flattened.
    NYA_Object* nested = nya_object_create(arena);
    NYA_Object* inner  = nya_object_create(arena);

    nya_object_add(inner, "deep", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"value" });
    nya_object_add(nested, "outer", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *inner });

    NYA_String* refused = nullptr;
    NYA_Error   error   = _nya_request_form_encode(arena, nested, &refused);

    nya_assert(!error.ok, "an object inside a form body is refused");
    nya_assert(nya_string_contains((NYA_ConstCString)error.message, "outer"), "naming the key that cannot go, got '%s'", (NYA_ConstCString)error.message);
  }

  printf("PASSED: test_request\n");
  return 0;
}
