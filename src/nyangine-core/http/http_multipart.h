/**
 * @file http_multipart.h
 *
 * The one body format an HTML `<input type=file>` sends: `multipart/form-data`, parsed as a stream of
 * parts over a byte range that is already whole in memory. Like http_message.h, everything here is a
 * pure function over bytes: it allocates nothing, reads nothing outside the range it is handed, and a
 * part it hands back points into that range rather than copying out of it — which is what keeps the
 * memory a request costs the request's own body and not a second copy of every file in it.
 *
 * ```
 * nya_http_multipart_boundary      the boundary out of a Content-Type header, validated
 * nya_http_multipart_reader_init   binds a reader to a body and its Content-Type, or refuses both
 * nya_http_multipart_next          the next part, "no more", or "malformed"
 * ```
 *
 * ```c
 * NYA_HttpMultipartReader reader = { 0 };
 * NYA_TRY(nya_http_multipart_reader_init(&reader, request->body, request->body_size,
 *                                        nya_http_request_header(request, "content-type")));
 *
 * NYA_HttpMultipartPart part = { 0 };
 * for (NYA_HttpMultipartStep step; (step = nya_http_multipart_next(&reader, &part)) == NYA_HTTP_MULTIPART_PART;) {
 *     // part.name is the field; part.filename is set on a file part; part.body is the bytes.
 * }
 * // step is NYA_HTTP_MULTIPART_DONE on a clean end, NYA_HTTP_MULTIPART_MALFORMED on anything else.
 * ```
 *
 * ── what it refuses, and why it refuses rather than repairs ──
 *
 * A multipart body is a stranger's bytes framed by a boundary the stranger also chose, so every reading
 * of it here is fail-closed: a boundary that is empty, longer than RFC 2046 allows, or made of bytes a
 * boundary may not carry; a part header block with no blank line ending it, longer than
 * NYA_HTTP_MULTIPART_MAX_PART_HEADER_BYTES, or with a bare CR or LF in it (the header-injection shape); a
 * part with no `Content-Disposition: form-data` name; a body with no closing boundary (the truncation a
 * dropped connection leaves); or more than NYA_HTTP_MULTIPART_MAX_PARTS parts. Any of those is
 * NYA_HTTP_MULTIPART_MALFORMED and the walk stops, because a framing this parser has lost the thread of
 * cannot be resynchronised without guessing, and guessing where the next part starts is the same class
 * of bug request smuggling is.
 *
 * ── what a part's fields are, and what they are not ──
 *
 * `name`, `filename` and `content_type` point into the body at the raw bytes between their quotes, not a
 * decoded copy: this parser does not unescape a quoted-string or percent-decode a filename, so a caller
 * that will use a filename validates it (a path separator, a control byte, a dot-segment) rather than
 * trusting it. That is the http_files.h facade's job, and it is why a filename here is a byte range and
 * never a path.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Longest boundary, terminator not included. RFC 2046 caps a boundary at 70 characters, so a longer one
 * is a body no conforming client sent and is refused rather than stored.
 * */
#define NYA_HTTP_MULTIPART_MAX_BOUNDARY 70

/**
 * Parts one body may carry. An upload form is one file and a field or two; sixteen is well past that and
 * still a bound on the work a single body can ask for, since each part is a scan for the next boundary.
 * */
#define NYA_HTTP_MULTIPART_MAX_PARTS 16

/**
 * Bytes one part's header block may take, its terminating blank line included. A `Content-Disposition`
 * with a long filename and a `Content-Type` fit inside this; a block that runs past it without a blank
 * line is a part with no end to its headers, which is refused rather than read into the body.
 * */
#define NYA_HTTP_MULTIPART_MAX_PART_HEADER_BYTES 512

// TYPES

typedef enum NYA_HttpMultipartStep     NYA_HttpMultipartStep;
typedef struct NYA_HttpMultipartPart   NYA_HttpMultipartPart;
typedef struct NYA_HttpMultipartReader NYA_HttpMultipartReader;

/** What one call to nya_http_multipart_next decided. */
enum NYA_HttpMultipartStep {
    /** `out_part` holds the next part. */
    NYA_HTTP_MULTIPART_PART = 0,

    /** The closing boundary was reached: every part has been handed back and nothing is wrong. */
    NYA_HTTP_MULTIPART_DONE,

    /** The bytes are not a well formed multipart body; see the file note for the ways. The walk is over. */
    NYA_HTTP_MULTIPART_MALFORMED,
};

/**
 * One part, as byte ranges into the body the reader was bound to. Every pointer is valid for as long as
 * that body is, and none is null terminated: the paired size says how far it goes.
 * */
struct NYA_HttpMultipartPart {
    /** The `name` of the `Content-Disposition: form-data`. Never null on a part: a nameless part is refused. */
    const char* name;
    u64         name_size;

    /** The `filename`, or null when the part declared none. A file part sets it; a plain field does not. */
    const char* filename;
    u64         filename_size;

    /** The part's own `Content-Type` value, or null when it declared none. */
    const char* content_type;
    u64         content_type_size;

    /** The part's bytes, between its header block and the next boundary. Size zero for an empty part. */
    const u8* body;
    u64       body_size;
};

/**
 * A walk in progress. Built by nya_http_multipart_reader_init and stepped by nya_http_multipart_next;
 * plain data with no allocation behind it, so a caller keeps one on the stack.
 * */
struct NYA_HttpMultipartReader {
    const u8* data;
    u64       size;
    u64       cursor;

    char boundary[NYA_HTTP_MULTIPART_MAX_BOUNDARY + 1];
    u64  boundary_size;

    u32 parts_seen;
    b8  started;
    b8  finished;
};

// FUNCTIONS

/**
 * The boundary out of a `multipart/form-data` Content-Type value, written to `out_boundary` and its
 * length to `out_size`.
 *
 * NYA_ERROR_INVALID_ARGUMENT when `content_type` is null, is not `multipart/form-data`, carries no
 * `boundary` parameter, or carries one that is empty, longer than NYA_HTTP_MULTIPART_MAX_BOUNDARY, or
 * made of a byte a boundary may not hold; NYA_ERROR_OUT_OF_MEMORY when it does not fit `capacity`. On any
 * error `out_boundary` is left holding an empty string and `out_size` zero.
 * */
NYA_API NYA_Error nya_http_multipart_boundary(NYA_ConstCString content_type, OUT char* out_boundary, u64 capacity, OUT u64* out_size)
    __attr_no_discard;

/**
 * Binds `reader` to `body` and the boundary in `content_type`. `reader` is zeroed first, so a reader
 * that failed to init is a clean DONE rather than a walk of stale bytes.
 *
 * The same NYA_ERROR_INVALID_ARGUMENT cases as nya_http_multipart_boundary, plus a null `body` with a
 * non-zero size. A zero-length body is accepted and walks to DONE with no parts.
 * */
NYA_API NYA_Error nya_http_multipart_reader_init(OUT NYA_HttpMultipartReader* reader, const u8* body, u64 body_size, NYA_ConstCString content_type)
    __attr_no_discard;

/**
 * The next part, or the reason there is not one.
 *
 * On NYA_HTTP_MULTIPART_PART, `out_part` is fully written. On NYA_HTTP_MULTIPART_DONE and
 * NYA_HTTP_MULTIPART_MALFORMED it is zeroed and every later call returns the same answer, so a loop that
 * ignores the difference simply stops. See NYA_HttpMultipartStep and the file note.
 * */
NYA_API NYA_HttpMultipartStep nya_http_multipart_next(NYA_HttpMultipartReader* reader, OUT NYA_HttpMultipartPart* out_part);
