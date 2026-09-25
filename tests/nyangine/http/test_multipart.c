/**
 * The multipart/form-data parser: a pure function over a body's bytes, so every case here is a literal
 * and a call. The hostile cases — a missing terminator, a bare LF in a part header, a boundary longer
 * than a boundary may be, a nameless part — outnumber the well formed ones, because a multipart body is
 * a stranger's bytes framed by a boundary the stranger also chose, and what matters is that a framing
 * this parser cannot trust is refused rather than half-read.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <string.h>

/** Binds a reader over a literal body and its Content-Type, asserting the init the caller expected to succeed. */
static void init_ok(NYA_HttpMultipartReader* reader, NYA_ConstCString content_type, NYA_ConstCString body) {
    NYA_EXPECT(nya_http_multipart_reader_init(reader, (const u8*)body, strlen(body), content_type));
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_multipart");
    defer      nya_arena_destroy(arena);
    (void)arena;

    // TEST: the boundary comes out of the Content-Type, quoted or not, and only from multipart/form-data
    {
        char boundary[NYA_HTTP_MULTIPART_MAX_BOUNDARY + 1] = { 0 };
        u64  size                                          = 0;

        NYA_EXPECT(nya_http_multipart_boundary("multipart/form-data; boundary=abc123", boundary, sizeof(boundary), &size));
        nya_check(strcmp(boundary, "abc123") == 0 && size == 6, "the unquoted boundary is read, got '%s'", boundary);

        NYA_EXPECT(nya_http_multipart_boundary("multipart/form-data; charset=utf-8; boundary=\"a b c\"", boundary, sizeof(boundary), &size));
        nya_check(strcmp(boundary, "a b c") == 0, "a quoted boundary keeps its spaces and is found after another parameter, got '%s'", boundary);

        // Case in the type is not significant, and the parameter is matched at a ';' so a value cannot be mistaken for it.
        NYA_EXPECT(nya_http_multipart_boundary("Multipart/Form-Data; boundary=XyZ", boundary, sizeof(boundary), &size));
        nya_check(strcmp(boundary, "XyZ") == 0, "the type is matched without case, got '%s'", boundary);

        nya_check(!nya_http_multipart_boundary("application/json", boundary, sizeof(boundary), &size).ok, "a non-multipart type is refused");
        nya_check(!nya_http_multipart_boundary("multipart/form-data", boundary, sizeof(boundary), &size).ok, "a body naming no boundary is refused");
        nya_check(
            !nya_http_multipart_boundary("multipart/form-data; boundary=", boundary, sizeof(boundary), &size).ok,
            "an empty boundary is refused"
        );
        nya_check(!nya_http_multipart_boundary(nullptr, boundary, sizeof(boundary), &size).ok, "no Content-Type at all is refused");
    }

    // TEST: a boundary longer than RFC 2046 allows is refused at init, not stored
    {
        char big[128] = { 0 };
        memset(big, 'a', 100);
        NYA_String* content_type = nya_string_sprintf(arena, "multipart/form-data; boundary=%s", big);

        NYA_HttpMultipartReader reader = { 0 };
        nya_check(
            !nya_http_multipart_reader_init(&reader, (const u8*)"", 0, nya_string_to_cstring(arena, content_type)).ok,
            "a 100 byte boundary is past the 70 a boundary may be"
        );
    }

    // TEST: a valid body of a field and a file walks to exactly those two parts, then a clean DONE
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=X";
        NYA_ConstCString body         = "--X\r\n"
                                        "Content-Disposition: form-data; name=\"note\"\r\n"
                                        "\r\n"
                                        "hello\r\n"
                                        "--X\r\n"
                                        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
                                        "Content-Type: text/plain\r\n"
                                        "\r\n"
                                        "the bytes\r\n"
                                        "--X--\r\n";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };

        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_PART, "the first part is a part");
        nya_check(part.name_size == 4 && memcmp(part.name, "note", 4) == 0, "its name is 'note'");
        nya_check(part.filename == nullptr, "a plain field has no filename");
        nya_check(part.body_size == 5 && memcmp(part.body, "hello", 5) == 0, "its body is exactly 'hello', the trailing CRLF eaten by the framing");

        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_PART, "the second part is a part");
        nya_check(part.name_size == 4 && memcmp(part.name, "file", 4) == 0, "its name is 'file'");
        nya_check(part.filename != nullptr && part.filename_size == 5 && memcmp(part.filename, "a.txt", 5) == 0, "its filename is 'a.txt'");
        nya_check(
            part.content_type != nullptr && part.content_type_size == 10 && memcmp(part.content_type, "text/plain", 10) == 0,
            "its content type is text/plain"
        );
        nya_check(part.body_size == 9 && memcmp(part.body, "the bytes", 9) == 0, "its body is the file bytes");

        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_DONE, "the closing boundary is a clean DONE");
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_DONE, "and a call after the end stays DONE");
    }

    // TEST: a body whose bytes contain the boundary text unquoted still ends at the real delimiter
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=BND";
        NYA_ConstCString body         = "--BND\r\n"
                                        "Content-Disposition: form-data; name=\"f\"; filename=\"x\"\r\n"
                                        "\r\n"
                                        "a--BNDb\r\n" // "--BND" inside the body, not at a CRLF delimiter
                                        "--BND--\r\n";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_PART, "the part is read");
        nya_check(
            part.body_size == 7 && memcmp(part.body, "a--BNDb", 7) == 0,
            "the boundary text inside the body is body, got " FMTu64 " bytes",
            part.body_size
        );
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_DONE, "and the real delimiter ends it");
    }

    // TEST: a body with no closing boundary is a truncation, refused rather than served short
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=X";
        NYA_ConstCString body         = "--X\r\n"
                                        "Content-Disposition: form-data; name=\"f\"; filename=\"x\"\r\n"
                                        "\r\n"
                                        "the bytes with no terminator";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_MALFORMED, "a missing terminator is malformed");
    }

    // TEST: a bare LF in a part's header block is the header-injection shape, refused
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=X";
        NYA_ConstCString body         = "--X\r\n"
                                        "Content-Disposition: form-data; name=\"f\"\nX-Injected: yes\r\n" // a lone LF, not CRLF
                                        "\r\n"
                                        "body\r\n"
                                        "--X--\r\n";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_MALFORMED, "a bare LF in the header block is malformed");
    }

    // TEST: a part with no Content-Disposition name is not a form-data part, refused
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=X";
        NYA_ConstCString body         = "--X\r\n"
                                        "Content-Type: text/plain\r\n"
                                        "\r\n"
                                        "orphan\r\n"
                                        "--X--\r\n";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_MALFORMED, "a nameless part is malformed");
    }

    // TEST: an empty file part (zero body bytes) is still a part, with its filename
    {
        NYA_ConstCString content_type = "multipart/form-data; boundary=X";
        NYA_ConstCString body         = "--X\r\n"
                                        "Content-Disposition: form-data; name=\"f\"; filename=\"empty.bin\"\r\n"
                                        "\r\n"
                                        "\r\n"
                                        "--X--\r\n";

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, content_type, body);

        NYA_HttpMultipartPart part = { 0 };
        nya_check(nya_http_multipart_next(&reader, &part) == NYA_HTTP_MULTIPART_PART, "an empty part is a part");
        nya_check(part.body_size == 0, "with a zero-length body");
        nya_check(part.filename != nullptr && part.filename_size == 9 && memcmp(part.filename, "empty.bin", 9) == 0, "and its filename");
    }

    // TEST: more parts than the bound are refused rather than walked without end
    {
        NYA_String* body = nya_string_create(arena);
        for (u32 index = 0; index <= NYA_HTTP_MULTIPART_MAX_PARTS; index++) {
            nya_string_extend(body, "--X\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nx\r\n");
        }
        nya_string_extend(body, "--X--\r\n");

        NYA_HttpMultipartReader reader = { 0 };
        init_ok(&reader, "multipart/form-data; boundary=X", nya_string_to_cstring(arena, body));

        NYA_HttpMultipartPart part = { 0 };
        NYA_HttpMultipartStep step = NYA_HTTP_MULTIPART_PART;
        u32                   seen = 0;
        while ((step = nya_http_multipart_next(&reader, &part)) == NYA_HTTP_MULTIPART_PART) seen++;

        nya_check(step == NYA_HTTP_MULTIPART_MALFORMED, "past the part bound the walk is refused");
        nya_check(seen == NYA_HTTP_MULTIPART_MAX_PARTS, "after exactly the bound's worth of parts, got " FMTu32, seen);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
