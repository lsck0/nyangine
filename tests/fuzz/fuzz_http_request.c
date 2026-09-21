/**
 * The HTTP request parser, fed whatever.
 *
 * This is the only code in the module that ever sees a byte a stranger chose, so it is the only one
 * that has to survive every byte a stranger could choose. The invariants asserted below are the ones
 * http_message.h promises on NYA_HTTP_PARSE_DONE, and everything downstream takes the parsed struct
 * on the strength of them.
 *
 * A refusal is the expected answer to hostile input and is not a finding. A crash, a read out of
 * bounds, an overflow, or an assertion reached from these bytes is.
 **/

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FUZZ_TARGET "http_request"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_http_request");
    defer      nya_arena_destroy(arena);

    /*
     * From the arena rather than the stack: NYA_HttpRequest is twenty kilobytes, and a persistent AFL
     * loop that put one on the stack per iteration would be measuring the stack rather than the parser.
     */
    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    if (request == nullptr) return;

    u64            consumed = 0;
    NYA_HttpStatus refusal  = NYA_HTTP_STATUS_NONE;

    NYA_HttpParse parsed = nya_http_request_parse(data, size, request, &consumed, &refusal);

    if (parsed == NYA_HTTP_PARSE_REFUSED) {
        // a refusal has to carry something to answer with, or the server has a status of zero to send.
        nya_assert(nya_http_status_is_valid(refusal), "the parser refused a request without saying what to answer");
        return;
    }

    if (parsed == NYA_HTTP_PARSE_INCOMPLETE) {
        nya_assert(consumed == 0, "an incomplete parse consumed bytes");
        return;
    }

    // everything from here is what http_message.h promises on DONE.
    nya_assert(consumed > 0 && consumed <= size, "the parser consumed nothing, or more than it was given");

    nya_assert(nya_http_method_is_valid(request->method), "a request parsed with no method");

    nya_assert(request->path[0] == '/', "a path that is not absolute reached a handler");
    nya_assert(request->body_size <= NYA_HTTP_MAX_BODY_BYTES, "a body larger than the bound parsed");
    nya_assert(request->header_count <= NYA_HTTP_MAX_HEADERS, "more headers parsed than the table holds");

    // every fixed buffer terminated inside itself, which is what lets everything downstream treat them
    // as C strings without carrying a length beside them.
    u64 path_length = 0;
    while (path_length < NYA_HTTP_MAX_PATH && request->path[path_length] != '\0') path_length++;
    nya_assert(path_length < NYA_HTTP_MAX_PATH, "the path is not terminated");

    u64 query_length = 0;
    while (query_length < NYA_HTTP_MAX_QUERY && request->query[query_length] != '\0') query_length++;
    nya_assert(query_length < NYA_HTTP_MAX_QUERY, "the query is not terminated");

    nya_assert(request->body[request->body_size] == '\0', "the body is not terminated one past its end");

    // no "." or ".." segment survived decoding, which is the traversal promise.
    for (u64 index = 0; index + 1 < path_length; index++) {
        if (request->path[index] != '/') continue;

        b8 dot     = request->path[index + 1] == '.' && (index + 2 == path_length || request->path[index + 2] == '/');
        b8 dot_dot = request->path[index + 1] == '.' && index + 2 < path_length && request->path[index + 2] == '.' &&
                     (index + 3 == path_length || request->path[index + 3] == '/');

        nya_assert(!dot && !dot_dot, "a relative segment survived the parser");
    }

    for (u32 index = 0; index < request->header_count; index++) {
        const NYA_HttpHeader* header = &request->headers[index];

        u64 name_length = 0;
        while (name_length < NYA_HTTP_MAX_HEADER_NAME && header->name[name_length] != '\0') name_length++;
        nya_assert(name_length > 0 && name_length < NYA_HTTP_MAX_HEADER_NAME, "a header name is empty or unterminated");

        u64 value_length = 0;
        while (value_length < NYA_HTTP_MAX_HEADER_VALUE && header->value[value_length] != '\0') value_length++;
        nya_assert(value_length < NYA_HTTP_MAX_HEADER_VALUE, "a header value is unterminated");

        // names are folded once, at the boundary, so nothing downstream has to fold again.
        for (u64 character = 0; character < name_length; character++) {
            nya_assert(header->name[character] < 'A' || header->name[character] > 'Z', "a header name reached a handler unfolded");
        }
    }

    /*
     * What a handler does with a request that parsed: read a header, read a query parameter, and try
     * the body as a document. All three take the parsed struct and none of them may be surprised by it.
     */
    (void)nya_http_request_header(request, "content-type");

    char value[64] = { 0 };
    (void)nya_http_request_query_param(request, "view", value, sizeof(value));

    NYA_Object* document = nullptr;
    (void)nya_http_request_json(request, arena, &document);

    /*
     * And what the server does with the answer: render a head for it. A request that parsed has to be
     * answerable, since the alternative is a connection that has nothing to send and cannot say why.
     */
    u8 body[64] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    u8  head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
    u64 head_size                              = 0;

    nya_assert(
        nya_http_response_head(&response, NYA_HTTP_STATUS_OK, request->keep_alive, head, sizeof(head), &head_size).ok,
        "a request that parsed could not be answered"
    );
}

#include "tests/fuzz/fuzz.h"
