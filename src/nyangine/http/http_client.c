#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_string.h"
#include "nyangine/http/http_client.h"
#include "nyangine/serde/serde.h"
#include "nyangine/serde/serde_nya_binary.h"
#include "nyangine/serde/serde_reflect.h"

// PRIVATE API DECLARATION

/**
 * A DTO to bytes, through its reflection and the wire format. The mirror of _nya_http_response_document_as
 * on the server, so the two produce the same bytes for the same value.
 * */
NYA_INTERNAL NYA_Error _nya_http_client_encode(
    NYA_Arena*                arena,
    const NYA_TypeReflection* type,
    const void*               dto,
    NYA_HttpMediaType         media,
    OUT const u8**            out_body,
    OUT u64*                  out_size
) __attr_no_discard;

/**
 * Bytes to a DTO, through the wire format and the reflection. The mirror of nya_http_request_reflect on the
 * server: any document format parses to the same object, and a binary body must carry this layout's hash.
 * */
NYA_INTERNAL NYA_Error _nya_http_client_decode(
    NYA_Arena*                arena,
    const u8*                 data,
    u64                       size,
    NYA_HttpMediaType         media,
    const NYA_TypeReflection* type,
    OUT void*                 out_dto
) __attr_no_discard;

/** Whether `status` is a 2xx, i.e. the server did what was asked. */
NYA_INTERNAL b8 _nya_http_client_ok(NYA_HttpStatus status) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_client_call(
    NYA_Arena*                     arena,
    const NYA_HttpClientTransport* transport,
    NYA_ConstCString               base_url,
    const NYA_HttpRoute*           route,
    const void*                    request_dto,
    OUT void*                      out_response_dto,
    OUT NYA_HttpClientResult*      out_result
) {
    nya_assert(arena != nullptr);
    nya_assert(transport != nullptr && transport->perform != nullptr, "a call needs a transport to reach the server");
    nya_assert(base_url != nullptr);
    nya_assert(route != nullptr);

    if (out_result != nullptr) *out_result = (NYA_HttpClientResult){ 0 };

    // The request must match the route it names: a body exactly when the route declares one, a place for the answer exactly when it answers with one; a caller mistake, caught here rather than sent for the server to reject.
    if (route->request_type != nullptr && request_dto == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the route declares a request body but none was given");
    }
    if (route->request_type == nullptr && request_dto != nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the route takes no request body but one was given");
    }
    if (route->response_type != nullptr && out_response_dto == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the route answers with a body but nowhere was given to put it");
    }

    // The URL is base plus the route's path, one trailing slash on the base dropped so it isn't doubled; a route is one absolute path matched exactly, no parameter — which instance travels in the request DTO, not the path. See the header.
    u64 base_length = strlen(base_url);
    if (base_length > 0 && base_url[base_length - 1] == '/') base_length--;

    NYA_String* url = nya_string_sprintf(arena, "%.*s%s", (s32)base_length, base_url, route->path);
    if (url == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the request URL could not be built");

    // NONE is JSON: what a client that never asked for a format gets, and what an outside integration reads.
    NYA_HttpMediaType format = transport->format == NYA_HTTP_MEDIA_NONE ? NYA_HTTP_MEDIA_JSON : transport->format;

    NYA_HttpClientWire wire = {
        .method       = route->method,
        .url          = (NYA_ConstCString)url->items,
        .content_type = NYA_HTTP_MEDIA_NONE,
        .accept       = route->response_type != nullptr ? format : NYA_HTTP_MEDIA_NONE,
    };

    if (route->request_type != nullptr) {
        NYA_TRY(_nya_http_client_encode(arena, route->request_type, request_dto, format, &wire.body, &wire.body_size));
        wire.content_type = format;
    }

    NYA_HttpClientReply reply = { 0 };
    NYA_TRY(transport->perform(transport->userdata, &wire, arena, &reply));

    if (out_result != nullptr) out_result->status = reply.status;

    // The reply's own format where the transport could tell, the one we asked for otherwise, JSON last.
    NYA_HttpMediaType reply_format = reply.media_type != NYA_HTTP_MEDIA_NONE ? reply.media_type : format;

    // A non-2xx status is a refusal, never a response DTO: it carries a NYA_HttpProblem read into the result, and out_response_dto is left untouched — surfacing it as an error rather than parsing the problem into the response type is the point.
    if (!_nya_http_client_ok(reply.status)) {
        if (out_result != nullptr && reply.body != nullptr && reply.body_size > 0) {
            NYA_HttpProblem problem  = { 0 };
            NYA_Error       decoded  = _nya_http_client_decode(arena, reply.body, reply.body_size, reply_format, nya_reflect_of(NYA_HttpProblem), &problem);
            if (decoded.ok) {
                out_result->problem        = problem;
                out_result->problem_parsed = true;
            }
        }

        return nya_error(NYA_ERROR_NOT_OK, "the server answered %d", (s32)reply.status);
    }

    // A clean 2xx: decode into a scratch value first and copy to the caller's DTO only once it parsed whole, so a body that doesn't fit is a clean error, never a half-written struct.
    if (route->response_type != nullptr) {
        void* decoded = nya_arena_alloc(arena, route->response_type->size);
        if (decoded == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to decode the response DTO");

        NYA_TRY(_nya_http_client_decode(arena, reply.body, reply.body_size, reply_format, route->response_type, decoded));

        memcpy(out_response_dto, decoded, route->response_type->size);
    }

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_http_client_encode(
    NYA_Arena*                arena,
    const NYA_TypeReflection* type,
    const void*               dto,
    NYA_HttpMediaType         media,
    OUT const u8**            out_body,
    OUT u64*                  out_size
) {
    nya_assert(arena != nullptr && type != nullptr && dto != nullptr && out_body != nullptr && out_size != nullptr);

    NYA_Object* object = nya_reflect_to_object(arena, type, dto);
    if (object == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the request DTO could not be described");

    NYA_String* body = nullptr;

    switch (media) {
        // No checksum: the native format carries one to guard a disk file against a torn write, but a request body has TCP underneath; answered the same way the server reads it.
        case NYA_HTTP_MEDIA_NYA:        body = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM); break;
        case NYA_HTTP_MEDIA_NYA_BINARY: NYA_TRY(nya_serde_nya_binary_encode(arena, object, type, &body)); break;
        default:                        body = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NO_CHECKSUM); break;
    }

    if (body == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the request DTO could not be serialized");

    *out_body = (const u8*)body->items;
    *out_size = body->length;

    return NYA_OK;
}

NYA_Error _nya_http_client_decode(
    NYA_Arena*                arena,
    const u8*                 data,
    u64                       size,
    NYA_HttpMediaType         media,
    const NYA_TypeReflection* type,
    OUT void*                 out_dto
) {
    nya_assert(arena != nullptr && type != nullptr && out_dto != nullptr);

    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_PARSE, "the response has no body to read the DTO from");

    NYA_Object* object = nullptr;

    switch (media) {
        case NYA_HTTP_MEDIA_NYA:        NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &object)); break;
        case NYA_HTTP_MEDIA_NYA_BINARY: NYA_TRY(nya_serde_nya_binary_decode(arena, data, size, type, &object)); break;
        default:                        NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &object)); break;
    }

    if (object == nullptr) return nya_error(NYA_ERROR_PARSE, "the response body is not an object");

    // Zeroed rather than left alone, like nya_http_request_reflect: an omitted field reads as zero, not as whatever the buffer held.
    memset(out_dto, 0, type->size);

    NYA_TRY(nya_reflect_from_object(type, out_dto, object));

    return NYA_OK;
}

b8 _nya_http_client_ok(NYA_HttpStatus status) {
    return status >= 200 && status < 300;
}
