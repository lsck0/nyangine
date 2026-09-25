#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/http/http_multipart.h"

// PRIVATE API DECLARATION

/** Lower cases one ASCII letter and leaves everything else, for a case-insensitive compare that never touches locale. */
NYA_INTERNAL char _nya_http_multipart_lower(char character) __attr_no_discard;

/** Whether `text[0, size)` equals the lower case literal `lower`, ignoring case. `lower` is already lower case. */
NYA_INTERNAL b8 _nya_http_multipart_ci_equals(const char* text, u64 size, NYA_ConstCString lower) __attr_no_discard;

/** Whether `text[0, size)` begins with the lower case literal `prefix`, ignoring case. */
NYA_INTERNAL b8 _nya_http_multipart_ci_starts_with(const char* text, u64 size, NYA_ConstCString prefix) __attr_no_discard;

/** The first offset at or after `from` where `needle` occurs in `hay`, or `hay_size` when it does not. */
NYA_INTERNAL u64 _nya_http_multipart_find(const u8* hay, u64 hay_size, const u8* needle, u64 needle_size, u64 from) __attr_no_discard;

/** Whether `character` may appear in a boundary: RFC 2046 bchars, space included (a trailing one is trimmed by the caller). */
NYA_INTERNAL b8 _nya_http_multipart_is_boundary_char(char character) __attr_no_discard;

/**
 * The value of the `attribute` parameter of a `Content-Disposition` header value `text[0, size)`, as a
 * range into it. Handles a quoted value (`name="x"`) and an unquoted token (`name=x`). False when the
 * attribute is absent; the value may legitimately be empty, which is true with a zero size.
 * */
NYA_INTERNAL b8
_nya_http_multipart_disposition_value(const char* text, u64 size, NYA_ConstCString attribute, OUT const char** out_value, OUT u64* out_size)
    __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_multipart_boundary(NYA_ConstCString content_type, OUT char* out_boundary, u64 capacity, OUT u64* out_size) {
    nya_assert(out_boundary != nullptr);
    nya_assert(out_size != nullptr);
    nya_assert(capacity > 0);

    out_boundary[0] = '\0';
    *out_size       = 0;

    if (content_type == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no Content-Type on a multipart body");

    u64 length = strlen(content_type);

    // The media type, up to the first ';', has to be multipart/form-data; a leading space is not allowed before the type, but the check ignores case,
    // which the grammar does.
    if (!_nya_http_multipart_ci_starts_with(content_type, length, "multipart/form-data")) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the body is not multipart/form-data");
    }

    // Walk the parameters after the type, looking for boundary=. A parameter name is matched at a ';' so a value that contains the text "boundary"
    // cannot be mistaken for the parameter.
    for (u64 index = 0; index < length; index++) {
        if (content_type[index] != ';') continue;

        u64 cursor = index + 1;
        while (cursor < length && (content_type[cursor] == ' ' || content_type[cursor] == '\t')) cursor++;

        if (!_nya_http_multipart_ci_starts_with(content_type + cursor, length - cursor, "boundary=")) continue;

        cursor += strlen("boundary=");

        // A quoted boundary drops the quotes; RFC 2046 allows one, and some clients send it.
        b8 quoted = cursor < length && content_type[cursor] == '"';
        if (quoted) cursor++;

        u64 start = cursor;
        while (cursor < length) {
            char character = content_type[cursor];
            if (quoted && character == '"') break;
            if (!quoted && (character == ';' || character == ' ' || character == '\t')) break;
            cursor++;
        }

        u64 size = cursor - start;

        if (size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the multipart boundary is empty");
        if (size > NYA_HTTP_MULTIPART_MAX_BOUNDARY)
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the multipart boundary is longer than %d bytes", NYA_HTTP_MULTIPART_MAX_BOUNDARY);
        if (size >= capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the multipart boundary does not fit " FMTu64 " bytes", capacity);

        for (u64 offset = 0; offset < size; offset++) {
            if (!_nya_http_multipart_is_boundary_char(content_type[start + offset])) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the multipart boundary has a byte a boundary may not carry");
            }
        }

        memcpy(out_boundary, content_type + start, size);
        out_boundary[size] = '\0';
        *out_size          = size;

        return NYA_OK;
    }

    return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the multipart body names no boundary");
}

NYA_Error nya_http_multipart_reader_init(OUT NYA_HttpMultipartReader* reader, const u8* body, u64 body_size, NYA_ConstCString content_type) {
    nya_assert(reader != nullptr);

    memset(reader, 0, sizeof(*reader));

    if (body == nullptr && body_size != 0)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a multipart body of " FMTu64 " bytes with no pointer", body_size);

    NYA_TRY(nya_http_multipart_boundary(content_type, reader->boundary, sizeof(reader->boundary), &reader->boundary_size));

    reader->data = body;
    reader->size = body_size;

    return NYA_OK;
}

NYA_HttpMultipartStep nya_http_multipart_next(NYA_HttpMultipartReader* reader, OUT NYA_HttpMultipartPart* out_part) {
    nya_assert(reader != nullptr);
    nya_assert(out_part != nullptr);

    memset(out_part, 0, sizeof(*out_part));

    if (reader->finished) return NYA_HTTP_MULTIPART_DONE;

    // An empty body carries no parts and is a clean end, not a malformed one: there was nothing to lose the thread of.
    if (reader->size == 0) {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_DONE;
    }

    // The dash-boundary, "--" and the boundary, is what every part is fenced by; it and the CRLF-prefixed form are the two tokens the walk hunts for.
    u8  dash[2 + NYA_HTTP_MULTIPART_MAX_BOUNDARY] = { 0 };
    u64 dash_size                                 = 2 + reader->boundary_size;
    dash[0]                                       = '-';
    dash[1]                                       = '-';
    memcpy(dash + 2, reader->boundary, reader->boundary_size);

    // The first part is opened by a dash-boundary that may follow a preamble; every later one by the CRLF the previous body ended against, already
    // consumed into the cursor. On the first call, find the opening dash-boundary and step past it.
    if (!reader->started) {
        u64 opening = _nya_http_multipart_find(reader->data, reader->size, dash, dash_size, 0);
        if (opening == reader->size) {
            reader->finished = true;
            return NYA_HTTP_MULTIPART_MALFORMED;
        }

        reader->cursor  = opening + dash_size;
        reader->started = true;
    }

    // After a dash-boundary come either "--" (the close) or optional linear whitespace and a CRLF (another part).
    u64 cursor = reader->cursor;

    if (cursor + 2 <= reader->size && reader->data[cursor] == '-' && reader->data[cursor + 1] == '-') {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_DONE;
    }

    while (cursor < reader->size && (reader->data[cursor] == ' ' || reader->data[cursor] == '\t')) cursor++;

    if (cursor + 2 > reader->size || reader->data[cursor] != '\r' || reader->data[cursor + 1] != '\n') {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_MALFORMED;
    }
    cursor += 2;

    if (reader->parts_seen >= NYA_HTTP_MULTIPART_MAX_PARTS) {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_MALFORMED;
    }

    // The header block runs to a blank line (a CRLF on its own). It is bounded, and only CRLF ends a line: a bare CR or LF is the header-injection
    // shape and is refused.
    u64 headers_start = cursor;
    u64 headers_end   = reader->size;

    const char* name              = nullptr;
    u64         name_size         = 0;
    const char* filename          = nullptr;
    u64         filename_size     = 0;
    const char* content_type      = nullptr;
    u64         content_type_size = 0;
    b8          has_disposition   = false;

    u64 line_start = cursor;
    while (true) {
        if (line_start - headers_start > NYA_HTTP_MULTIPART_MAX_PART_HEADER_BYTES) {
            reader->finished = true;
            return NYA_HTTP_MULTIPART_MALFORMED;
        }

        // The end of this line, at its CRLF. A CR without a following LF, or an LF with no leading CR, is malformed framing.
        u64 line_end = line_start;
        while (line_end < reader->size && reader->data[line_end] != '\r' && reader->data[line_end] != '\n') line_end++;

        if (line_end + 1 >= reader->size || reader->data[line_end] != '\r' || reader->data[line_end + 1] != '\n') {
            reader->finished = true;
            return NYA_HTTP_MULTIPART_MALFORMED;
        }

        u64 line_size = line_end - line_start;

        // A blank line ends the header block; the body starts after its CRLF.
        if (line_size == 0) {
            headers_end = line_end + 2;
            break;
        }

        const char* line = (const char*)reader->data + line_start;

        // Only the two headers this parser reads are looked at; any other is skipped, not refused, so a client's extra part header is not a reason to
        // fail an upload.
        u64 colon = 0;
        while (colon < line_size && line[colon] != ':') colon++;

        if (colon < line_size) {
            const char* value      = line + colon + 1;
            u64         value_size = line_size - colon - 1;

            // Trim the optional whitespace after the colon.
            while (value_size > 0 && (value[0] == ' ' || value[0] == '\t')) {
                value++;
                value_size--;
            }
            while (value_size > 0 && (value[value_size - 1] == ' ' || value[value_size - 1] == '\t')) value_size--;

            if (_nya_http_multipart_ci_equals(line, colon, "content-disposition")) {
                has_disposition = true;
                (void)_nya_http_multipart_disposition_value(value, value_size, "name", &name, &name_size);
                (void)_nya_http_multipart_disposition_value(value, value_size, "filename", &filename, &filename_size);
            } else if (_nya_http_multipart_ci_equals(line, colon, "content-type")) {
                content_type      = value;
                content_type_size = value_size;
            }
        }

        line_start = line_end + 2;
    }

    // A part with no Content-Disposition name is not a form-data part; refuse it rather than hand back a nameless one.
    if (!has_disposition || name == nullptr) {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_MALFORMED;
    }

    // The body runs to the next boundary, which is CRLF then the dash-boundary. Its absence is a truncated body, refused rather than served short.
    u8  crlf_dash[2 + 2 + NYA_HTTP_MULTIPART_MAX_BOUNDARY] = { 0 };
    u64 crlf_dash_size                                     = 2 + dash_size;
    crlf_dash[0]                                           = '\r';
    crlf_dash[1]                                           = '\n';
    memcpy(crlf_dash + 2, dash, dash_size);

    u64 delimiter = _nya_http_multipart_find(reader->data, reader->size, crlf_dash, crlf_dash_size, headers_end);
    if (delimiter == reader->size) {
        reader->finished = true;
        return NYA_HTTP_MULTIPART_MALFORMED;
    }

    out_part->name              = name;
    out_part->name_size         = name_size;
    out_part->filename          = filename;
    out_part->filename_size     = filename_size;
    out_part->content_type      = content_type;
    out_part->content_type_size = content_type_size;
    out_part->body              = reader->data + headers_end;
    out_part->body_size         = delimiter - headers_end;

    // Past the CRLF and the dash-boundary, ready for the "--" or CRLF that says close or continue.
    reader->cursor = delimiter + crlf_dash_size;
    reader->parts_seen++;

    return NYA_HTTP_MULTIPART_PART;
}

// PRIVATE API IMPLEMENTATION

char _nya_http_multipart_lower(char character) {
    if (character >= 'A' && character <= 'Z') return (char)(character - 'A' + 'a');
    return character;
}

b8 _nya_http_multipart_ci_equals(const char* text, u64 size, NYA_ConstCString lower) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(lower != nullptr);

    if (strlen(lower) != size) return false;

    for (u64 index = 0; index < size; index++) {
        if (_nya_http_multipart_lower(text[index]) != lower[index]) return false;
    }

    return true;
}

b8 _nya_http_multipart_ci_starts_with(const char* text, u64 size, NYA_ConstCString prefix) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(prefix != nullptr);

    u64 prefix_size = strlen(prefix);
    if (prefix_size > size) return false;

    for (u64 index = 0; index < prefix_size; index++) {
        if (_nya_http_multipart_lower(text[index]) != prefix[index]) return false;
    }

    return true;
}

u64 _nya_http_multipart_find(const u8* hay, u64 hay_size, const u8* needle, u64 needle_size, u64 from) {
    nya_assert(needle != nullptr);
    nya_assert(needle_size > 0);

    if (needle_size > hay_size) return hay_size;

    for (u64 index = from; index + needle_size <= hay_size; index++) {
        if (memcmp(hay + index, needle, needle_size) == 0) return index;
    }

    return hay_size;
}

b8 _nya_http_multipart_is_boundary_char(char character) {
    if (character >= 'a' && character <= 'z') return true;
    if (character >= 'A' && character <= 'Z') return true;
    if (character >= '0' && character <= '9') return true;

    switch (character) {
        case '\'':
        case '(':
        case ')':
        case '+':
        case '_':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case '=':
        case '?':
        case ' ':  return true;
        default:   return false;
    }
}

b8 _nya_http_multipart_disposition_value(const char* text, u64 size, NYA_ConstCString attribute, OUT const char** out_value, OUT u64* out_size) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(attribute != nullptr);

    u64 attribute_size = strlen(attribute);

    for (u64 index = 0; index < size; index++) {
        // A parameter starts after a ';'. Match the attribute name at a boundary so "filename" is not found inside another parameter's value, and so
        // "name" does not match the tail of "filename".
        if (index != 0 && text[index - 1] != ';') continue;

        u64 cursor = index;
        while (cursor < size && (text[cursor] == ' ' || text[cursor] == '\t')) cursor++;

        if (!_nya_http_multipart_ci_starts_with(text + cursor, size - cursor, attribute)) continue;
        cursor += attribute_size;

        // The attribute name has to be followed by '=', not be the prefix of a longer one (name vs name*).
        if (cursor >= size || text[cursor] != '=') continue;
        cursor++;

        b8 quoted = cursor < size && text[cursor] == '"';
        if (quoted) cursor++;

        u64 start = cursor;
        while (cursor < size) {
            char character = text[cursor];
            if (quoted && character == '"') break;
            if (!quoted && (character == ';' || character == ' ' || character == '\t')) break;
            cursor++;
        }

        *out_value = text + start;
        *out_size  = cursor - start;

        return true;
    }

    return false;
}
