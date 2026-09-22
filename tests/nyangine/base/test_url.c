/**
 * The URL parser against the edge cases people get wrong: every rule it refuses with, at the byte it
 * names, and the shapes it has to accept. The laws (round trips) are in tests/nyangine/testing/test_property.c
 * and arbitrary bytes are tests/fuzz/fuzz_url.c's job.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Which of the two parse calls a case goes through. PARSE_ prefixed, since wingdi.h defines ABSOLUTE. */
typedef enum {
    PARSE_ABSOLUTE,
    PARSE_TARGET,
} ParseKind;

/** An input that must be refused, with the rule and the byte it must be refused at. */
typedef struct {
    ParseKind        kind;
    NYA_ConstCString text;
    NYA_UrlRule      rule;
    u32              offset;
} Refusal;

/** An input that must parse, what it must parse to, and what it must render back as. */
typedef struct {
    ParseKind        kind;
    NYA_ConstCString text;
    NYA_UrlScheme    scheme;
    NYA_UrlHostKind  host_kind;
    NYA_ConstCString host;
    u16              port;
    NYA_ConstCString path;
    NYA_ConstCString query;
    NYA_ConstCString fragment;
    b8               has_userinfo;
    NYA_ConstCString formatted;
} Acceptance;

static NYA_Error parse(ParseKind kind, NYA_ConstCString text, u64 size, NYA_Url* url, NYA_UrlFailure* failure) {
    return kind == PARSE_ABSOLUTE ? nya_url_parse(text, size, url, failure) : nya_url_parse_target(text, size, url, failure);
}

/** Whether a component holds exactly `expected`; null means the component must be absent. */
static b8 span_is(const NYA_Url* url, b8 present, NYA_UrlSpan span, NYA_ConstCString expected) {
    if (expected == nullptr) return !present && span.length == 0;

    u64 length = strlen(expected);

    return present && span.length == length && nya_memcmp(url->text + span.offset, expected, length) == 0;
}

static const Refusal REFUSALS[] = {
    { PARSE_ABSOLUTE, "",                             NYA_URL_RULE_EMPTY,               0  },
    { PARSE_ABSOLUTE, "example.com",                  NYA_URL_RULE_SCHEME_MISSING,      0  },
    { PARSE_ABSOLUTE, "://h",                         NYA_URL_RULE_SCHEME_MISSING,      0  },
    { PARSE_ABSOLUTE, "1http://h",                    NYA_URL_RULE_SCHEME_MISSING,      0  },
    { PARSE_ABSOLUTE, "ftp://h/",                     NYA_URL_RULE_SCHEME_UNKNOWN,      0  },
    { PARSE_ABSOLUTE, "javascript://h/",              NYA_URL_RULE_SCHEME_UNKNOWN,      0  },
    { PARSE_ABSOLUTE, "http:h",                       NYA_URL_RULE_AUTHORITY_MISSING,   5  },
    { PARSE_ABSOLUTE, "http:/h",                      NYA_URL_RULE_AUTHORITY_MISSING,   5  },
    { PARSE_ABSOLUTE, "http://",                      NYA_URL_RULE_HOST_EMPTY,          7  },
    { PARSE_ABSOLUTE, "http://:80/",                  NYA_URL_RULE_HOST_EMPTY,          7  },
    { PARSE_ABSOLUTE, "http://u@/",                   NYA_URL_RULE_HOST_EMPTY,          9  },
    { PARSE_ABSOLUTE, "http://h:0/",                  NYA_URL_RULE_PORT_OUT_OF_RANGE,   9  },
    { PARSE_ABSOLUTE, "http://h:65536/",              NYA_URL_RULE_PORT_OUT_OF_RANGE,   9  },
    { PARSE_ABSOLUTE, "http://h:99999999999999999/",  NYA_URL_RULE_PORT_OUT_OF_RANGE,   9  },
    { PARSE_ABSOLUTE, "http://h:/",                   NYA_URL_RULE_PORT_MALFORMED,      8  },
    { PARSE_ABSOLUTE, "http://h:080/",                NYA_URL_RULE_PORT_MALFORMED,      9  },
    { PARSE_ABSOLUTE, "http://h:8a/",                 NYA_URL_RULE_PORT_MALFORMED,      10 },
    { PARSE_ABSOLUTE, "http://h:-1/",                 NYA_URL_RULE_PORT_MALFORMED,      9  },
    { PARSE_ABSOLUTE, "http://h/a b",                 NYA_URL_RULE_SPACE,               10 },
    { PARSE_ABSOLUTE, " http://h/",                   NYA_URL_RULE_SPACE,               0  },
    { PARSE_ABSOLUTE, "http://h/a\tb",                NYA_URL_RULE_CONTROL_CHARACTER,   10 },
    { PARSE_ABSOLUTE, "http://h/\r\nX: y",            NYA_URL_RULE_CONTROL_CHARACTER,   9  },
    { PARSE_ABSOLUTE, "http://h/\x7f",                NYA_URL_RULE_CONTROL_CHARACTER,   9  },
    { PARSE_ABSOLUTE, "http://h/%zz",                 NYA_URL_RULE_PERCENT_MALFORMED,   9  },
    { PARSE_ABSOLUTE, "http://h/%2",                  NYA_URL_RULE_PERCENT_MALFORMED,   9  },
    { PARSE_ABSOLUTE, "http://h/?q=%",                NYA_URL_RULE_PERCENT_MALFORMED,   12 },
    { PARSE_ABSOLUTE, "http://h/#%4",                 NYA_URL_RULE_PERCENT_MALFORMED,   10 },
    { PARSE_ABSOLUTE, "http://h/?q=%00",              NYA_URL_RULE_CONTROL_CHARACTER,   12 },
    { PARSE_ABSOLUTE, "http://h/a%0Ab",               NYA_URL_RULE_CONTROL_CHARACTER,   10 },
    { PARSE_ABSOLUTE, "http://h/a%2Fb",               NYA_URL_RULE_PATH_ENCODED_SLASH,  10 },
    { PARSE_ABSOLUTE, "http://h/a%2fb",               NYA_URL_RULE_PATH_ENCODED_SLASH,  10 },
    { PARSE_ABSOLUTE, "http://h/../x",                NYA_URL_RULE_PATH_DOT_SEGMENT,    9  },
    { PARSE_ABSOLUTE, "http://h/a/./x",               NYA_URL_RULE_PATH_DOT_SEGMENT,    11 },
    { PARSE_ABSOLUTE, "http://h/a/..",                NYA_URL_RULE_PATH_DOT_SEGMENT,    11 },
    { PARSE_ABSOLUTE, "http://h/%2E%2e/x",            NYA_URL_RULE_PATH_DOT_SEGMENT,    9  },
    { PARSE_ABSOLUTE, "http://h//x",                  NYA_URL_RULE_PATH_DOUBLE_SLASH,   8  },
    { PARSE_ABSOLUTE, "http://[::1/",                 NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[]/",                   NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[1:2:3:4:5:6:7:8:9]/",  NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[1:2:3:4:5:6:7]/",      NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[::1::]/",              NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[12345::]/",            NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[1:]/",                 NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[:1]/",                 NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[::1.2.3]/",            NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[fe80::1%25eth0]/",     NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[v1.x]/",               NYA_URL_RULE_IPV6_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://[::1]x/",               NYA_URL_RULE_HOST_MALFORMED,      12 },
    { PARSE_ABSOLUTE, "http://::1/",                  NYA_URL_RULE_HOST_EMPTY,          7  },
    { PARSE_ABSOLUTE, "http://1.2.3/",                NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://2130706433/",           NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://0x7f.1/",               NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://010.0.0.1/",            NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://256.0.0.1/",            NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://1.2.3.4.5/",            NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://a..b/",                 NYA_URL_RULE_HOST_MALFORMED,      9  },
    { PARSE_ABSOLUTE, "http://h./",                   NYA_URL_RULE_HOST_MALFORMED,      9  },
    { PARSE_ABSOLUTE, "http://-h/",                   NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://h-/",                   NYA_URL_RULE_HOST_MALFORMED,      7  },
    { PARSE_ABSOLUTE, "http://h%41/",                 NYA_URL_RULE_HOST_MALFORMED,      8  },
    { PARSE_ABSOLUTE, "http://a@b@c/",                NYA_URL_RULE_HOST_MALFORMED,      10 },
    { PARSE_ABSOLUTE, "http://h/{}",                  NYA_URL_RULE_CHARACTER,           9  },
    { PARSE_ABSOLUTE, "http://h/\xc3\xa9",            NYA_URL_RULE_CHARACTER,           9  },
    { PARSE_ABSOLUTE, "http://h\\x",                  NYA_URL_RULE_CHARACTER,           8  },
    { PARSE_ABSOLUTE, "http://h/[x]",                 NYA_URL_RULE_CHARACTER,           9  },
    { PARSE_ABSOLUTE, "http://h/?a[]=1",              NYA_URL_RULE_CHARACTER,           11 },
    { PARSE_ABSOLUTE, "http://h/#a#b",                NYA_URL_RULE_CHARACTER,           11 },
    { PARSE_ABSOLUTE, "http://u[@h/",                 NYA_URL_RULE_CHARACTER,           8  },
    { PARSE_TARGET,   "",                             NYA_URL_RULE_EMPTY,               0  },
    { PARSE_TARGET,   "a",                            NYA_URL_RULE_PATH_NOT_ABSOLUTE,   0  },
    { PARSE_TARGET,   "*",                            NYA_URL_RULE_PATH_NOT_ABSOLUTE,   0  },
    { PARSE_TARGET,   "?a=1",                         NYA_URL_RULE_PATH_NOT_ABSOLUTE,   0  },
    { PARSE_TARGET,   "http://h/",                    NYA_URL_RULE_PATH_NOT_ABSOLUTE,   0  },
    { PARSE_TARGET,   "//h/x",                        NYA_URL_RULE_PATH_DOUBLE_SLASH,   0  },
    { PARSE_TARGET,   "/a#b",                         NYA_URL_RULE_CHARACTER,           2  },
    { PARSE_TARGET,   "/a?b#c",                       NYA_URL_RULE_CHARACTER,           4  },
    { PARSE_TARGET,   "/a%2F..%2Fetc",                NYA_URL_RULE_PATH_ENCODED_SLASH,  2  },
    { PARSE_TARGET,   "/%2e%2e/etc",                  NYA_URL_RULE_PATH_DOT_SEGMENT,    1  },
    { PARSE_TARGET,   "/a%00",                        NYA_URL_RULE_CONTROL_CHARACTER,   2  },
};

static const Acceptance ACCEPTANCES[] = {
    // an empty path: nothing after the authority at all.
    { PARSE_ABSOLUTE, "http://example.com", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "example.com", 0, "", nullptr, nullptr, false, "http://example.com" },
    { PARSE_ABSOLUTE, "http://h?", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "", "", nullptr, false, "http://h?" },
    { PARSE_ABSOLUTE, "https://Example.COM:8443/a/b?x=1#top", NYA_URL_SCHEME_HTTPS, NYA_URL_HOST_NAME, "Example.COM", 8443, "/a/b", "x=1", "top", false,
     "https://Example.COM:8443/a/b?x=1#top" },
    // a scheme is case insensitive and renders lower case; the host keeps the case it came in.
    { PARSE_ABSOLUTE, "HTTP://h/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "/", nullptr, nullptr, false, "http://h/" },
    { PARSE_ABSOLUTE, "wss://h:65535/", NYA_URL_SCHEME_WSS, NYA_URL_HOST_NAME, "h", 65535, "/", nullptr, nullptr, false, "wss://h:65535/" },
    { PARSE_ABSOLUTE, "ws://my_service.internal/", NYA_URL_SCHEME_WS, NYA_URL_HOST_NAME, "my_service.internal", 0, "/", nullptr, nullptr, false,
     "ws://my_service.internal/" },
    // IPv6 in brackets, stored without them, written back with them.
    { PARSE_ABSOLUTE, "http://[::1]:8080/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV6, "::1", 8080, "/", nullptr, nullptr, false, "http://[::1]:8080/" },
    { PARSE_ABSOLUTE, "http://[::]/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV6, "::", 0, "/", nullptr, nullptr, false, "http://[::]/" },
    { PARSE_ABSOLUTE, "http://[2001:db8::1]", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV6, "2001:db8::1", 0, "", nullptr, nullptr, false, "http://[2001:db8::1]" },
    { PARSE_ABSOLUTE, "http://[1:2:3:4:5:6:7:8]/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV6, "1:2:3:4:5:6:7:8", 0, "/", nullptr, nullptr, false,
     "http://[1:2:3:4:5:6:7:8]/" },
    { PARSE_ABSOLUTE, "http://[::ffff:192.0.2.1]/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV6, "::ffff:192.0.2.1", 0, "/", nullptr, nullptr, false,
     "http://[::ffff:192.0.2.1]/" },
    { PARSE_ABSOLUTE, "http://192.168.0.1:80/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV4, "192.168.0.1", 80, "/", nullptr, nullptr, false,
     "http://192.168.0.1:80/" },
    { PARSE_ABSOLUTE, "http://0.0.0.0/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_IPV4, "0.0.0.0", 0, "/", nullptr, nullptr, false, "http://0.0.0.0/" },
    // trailing '?' and '#' are an empty query and fragment, not missing ones, and survive a round trip.
    { PARSE_ABSOLUTE, "ws://h/?#", NYA_URL_SCHEME_WS, NYA_URL_HOST_NAME, "h", 0, "/", "", "", false, "ws://h/?#" },
    { PARSE_ABSOLUTE, "http://h/#", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "/", nullptr, "", false, "http://h/#" },
    // '?' and '/' are data inside a query and a fragment.
    { PARSE_ABSOLUTE, "http://h/p?a=/b?c#d/e?f", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "/p", "a=/b?c", "d/e?f", false,
     "http://h/p?a=/b?c#d/e?f" },
    // userinfo is noticed and never written back.
    { PARSE_ABSOLUTE, "http://user:pass@h/", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "/", nullptr, nullptr, true, "http://h/" },
    // "..x" and "%2e%2ex" are names, not climbs.
    { PARSE_ABSOLUTE, "http://h/..x/%2e%2ex", NYA_URL_SCHEME_HTTP, NYA_URL_HOST_NAME, "h", 0, "/..x/%2e%2ex", nullptr, nullptr, false, "http://h/..x/%2e%2ex" },
    { PARSE_TARGET, "/", NYA_URL_SCHEME_NONE, NYA_URL_HOST_NONE, nullptr, 0, "/", nullptr, nullptr, false, "/" },
    { PARSE_TARGET, "/a/", NYA_URL_SCHEME_NONE, NYA_URL_HOST_NONE, nullptr, 0, "/a/", nullptr, nullptr, false, "/a/" },
    { PARSE_TARGET, "/a+b/%20?q=a+b", NYA_URL_SCHEME_NONE, NYA_URL_HOST_NONE, nullptr, 0, "/a+b/%20", "q=a+b", nullptr, false, "/a+b/%20?q=a+b" },
    { PARSE_TARGET, "/api?", NYA_URL_SCHEME_NONE, NYA_URL_HOST_NONE, nullptr, 0, "/api", "", nullptr, false, "/api?" },
};

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("TEST: every rule refuses at the byte it names\n");
    for (u32 i = 0; i < nya_carray_length(REFUSALS); i++) {
        const Refusal* refusal = &REFUSALS[i];

        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };
        NYA_Error      result  = parse(refusal->kind, refusal->text, strlen(refusal->text), &url, &failure);

        nya_assert(!result.ok && result.kind == NYA_ERROR_PARSE, "'%s' parsed", refusal->text);
        nya_assert(
            failure.rule == refusal->rule && failure.offset == refusal->offset,
            "'%s' was refused for %s at %u, not %s at %u",
            refusal->text,
            nya_url_rule_text(failure.rule),
            failure.offset,
            nya_url_rule_text(refusal->rule),
            refusal->offset
        );
        nya_assert(nya_is_zeroed(url), "a refusal left a half filled url behind");
    }

    printf("TEST: the shapes that must parse, and what they render back as\n");
    for (u32 i = 0; i < nya_carray_length(ACCEPTANCES); i++) {
        const Acceptance* expected = &ACCEPTANCES[i];

        NYA_Url   url    = { 0 };
        NYA_Error result = parse(expected->kind, expected->text, strlen(expected->text), &url, nullptr);

        nya_assert(result.ok, "'%s' was refused: %s", expected->text, (NYA_ConstCString)result.message);

        nya_assert(url.scheme == expected->scheme && url.host_kind == expected->host_kind, "'%s'", expected->text);
        nya_assert(span_is(&url, expected->host != nullptr, url.host, expected->host), "'%s': host", expected->text);
        nya_assert(url.has_port == (expected->port != 0) && url.port == expected->port, "'%s': port", expected->text);
        nya_assert(span_is(&url, true, url.path, expected->path), "'%s': path", expected->text);
        nya_assert(span_is(&url, url.has_query, url.query, expected->query), "'%s': query", expected->text);
        nya_assert(span_is(&url, url.has_fragment, url.fragment, expected->fragment), "'%s': fragment", expected->text);
        nya_assert(url.has_userinfo == expected->has_userinfo, "'%s': userinfo", expected->text);

        char formatted[NYA_URL_MAX_BYTES + 1];
        u64  length = 0;

        NYA_EXPECT(nya_url_format(&url, formatted, sizeof(formatted), &length));
        nya_assert(nya_string_equals(formatted, expected->formatted), "'%s' rendered as '%s'", expected->text, formatted);
        nya_assert(length == strlen(expected->formatted));
    }

    printf("TEST: overlong input is refused before a byte of it is read\n");
    {
        // room past the bound, so the too long cases below hand over a size that is really there.
        static char long_url[NYA_URL_MAX_BYTES + 16];

        // exactly at the bound is fine; one byte past it is not.
        nya_memset(long_url, 'a', sizeof(long_url));
        nya_memcpy(long_url, "http://h/", 9);

        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };

        NYA_EXPECT(nya_url_parse(long_url, NYA_URL_MAX_BYTES, &url, &failure));
        nya_assert(url.path.length == NYA_URL_MAX_BYTES - 8);

        nya_assert(!nya_url_parse(long_url, NYA_URL_MAX_BYTES + 1, &url, &failure).ok);
        nya_assert(failure.rule == NYA_URL_RULE_TOO_LONG && failure.offset == NYA_URL_MAX_BYTES);

        nya_assert(!nya_url_parse_target(long_url + 8, NYA_URL_MAX_BYTES + 1, &url, &failure).ok);
        nya_assert(failure.rule == NYA_URL_RULE_TOO_LONG);

        // a host past DNS's limit, in labels that are each fine.
        char long_host[16 + NYA_URL_HOST_MAX_BYTES + 8] = "http://";
        u64  at                                         = 7 + NYA_URL_HOST_MAX_BYTES + 1;

        for (u64 i = 7; i < at; i++) long_host[i] = i % 32 == 0 ? '.' : 'a';
        long_host[at++] = '/';
        long_host[at]   = '\0';

        nya_assert(!nya_url_parse(long_host, at, &url, &failure).ok);
        nya_assert(failure.rule == NYA_URL_RULE_HOST_TOO_LONG && failure.offset == 7);

        // one label past 63.
        char long_label[8 + NYA_URL_HOST_LABEL_MAX_BYTES + 4] = "http://";
        nya_memset(long_label + 7, 'a', NYA_URL_HOST_LABEL_MAX_BYTES + 1);
        long_label[7 + NYA_URL_HOST_LABEL_MAX_BYTES + 1] = '\0';

        nya_assert(!nya_url_parse(long_label, strlen(long_label), &url, &failure).ok);
        nya_assert(failure.rule == NYA_URL_RULE_HOST_MALFORMED);
    }

    printf("TEST: the path decodes into what a router matches, '+' included as itself\n");
    {
        NYA_Url url = { 0 };
        NYA_EXPECT(nya_url_parse_target("/a+b/c%20d/%C3%A9", 17, &url, nullptr));

        char path[64];
        u64  length = 0;

        NYA_EXPECT(nya_url_path_decode(&url, path, sizeof(path), &length));
        nya_assert(nya_string_equals(path, "/a+b/c d/\xc3\xa9"), "a '+' is a space only in a query");
        nya_assert(length == strlen(path));

        // a buffer too small is refused rather than cut short.
        char small[4];
        NYA_Error result = nya_url_path_decode(&url, small, sizeof(small), &length);
        nya_assert(!result.ok && result.kind == NYA_ERROR_OUT_OF_MEMORY);
        nya_assert(small[0] == '\0');
    }

    printf("TEST: query parameters decode, '+' is a space, and a repeated name is refused\n");
    {
        NYA_ConstCString text = "/s?q=a+b%2Bc&empty=&flag&a%20b=1&&page=2&page=3&";
        NYA_Url          url  = { 0 };
        NYA_EXPECT(nya_url_parse_target(text, strlen(text), &url, nullptr));

        char value[32];
        b8   found = false;

        NYA_EXPECT(nya_url_query_find(&url, "q", value, sizeof(value), &found));
        nya_assert(found && nya_string_equals(value, "a b+c"));

        NYA_EXPECT(nya_url_query_find(&url, "empty", value, sizeof(value), &found));
        nya_assert(found && value[0] == '\0', "an empty value is present and empty");

        NYA_EXPECT(nya_url_query_find(&url, "flag", value, sizeof(value), &found));
        nya_assert(found && value[0] == '\0', "a key with no '=' is present and empty");

        NYA_EXPECT(nya_url_query_find(&url, "a b", value, sizeof(value), &found));
        nya_assert(found && nya_string_equals(value, "1"), "keys are compared decoded");

        NYA_EXPECT(nya_url_query_find(&url, "absent", value, sizeof(value), &found));
        nya_assert(!found && value[0] == '\0');

        // prefixes are not matches.
        NYA_EXPECT(nya_url_query_find(&url, "pag", value, sizeof(value), &found));
        nya_assert(!found);

        NYA_Error twice = nya_url_query_find(&url, "page", value, sizeof(value), &found);
        nya_assert(!twice.ok && twice.kind == NYA_ERROR_PARSE && !found && value[0] == '\0');

        char small[3];
        NYA_Error short_buffer = nya_url_query_find(&url, "q", small, sizeof(small), &found);
        nya_assert(!short_buffer.ok && short_buffer.kind == NYA_ERROR_OUT_OF_MEMORY && !found && small[0] == '\0');

        // no query at all is no parameters, not an error.
        NYA_EXPECT(nya_url_parse_target("/s", 2, &url, nullptr));
        NYA_EXPECT(nya_url_query_find(&url, "q", value, sizeof(value), &found));
        nya_assert(!found);
    }

    printf("TEST: percent encoding escapes everything but the unreserved set\n");
    {
        NYA_ConstCString raw = "a b/~._-\xc3\xa9%+";
        char             encoded[64];
        u64              length = 0;

        NYA_EXPECT(nya_percent_encode((const u8*)raw, strlen(raw), encoded, sizeof(encoded), &length));
        nya_assert(nya_string_equals(encoded, "a%20b%2F~._-%C3%A9%25%2B"), "encoded as '%s'", encoded);
        nya_assert(length == strlen(encoded));

        u8  decoded[64];
        u64 decoded_length = 0;

        NYA_EXPECT(nya_percent_decode(encoded, length, decoded, sizeof(decoded), &decoded_length));
        nya_assert(decoded_length == strlen(raw) && nya_memcmp(decoded, raw, decoded_length) == 0);

        // lower case escapes decode too, and '+' is itself outside a query.
        NYA_EXPECT(nya_percent_decode("%4a+%00", 7, decoded, sizeof(decoded), &decoded_length));
        nya_assert(decoded_length == 3 && decoded[0] == 'J' && decoded[1] == '+' && decoded[2] == 0);

        NYA_Error half = nya_percent_decode("ab%4", 4, decoded, sizeof(decoded), &decoded_length);
        nya_assert(!half.ok && half.kind == NYA_ERROR_PARSE && decoded_length == 0);

        NYA_Error not_hex = nya_percent_decode("%GG", 3, decoded, sizeof(decoded), &decoded_length);
        nya_assert(!not_hex.ok && not_hex.kind == NYA_ERROR_PARSE);

        NYA_Error short_decode = nya_percent_decode("abc", 3, decoded, 2, &decoded_length);
        nya_assert(!short_decode.ok && short_decode.kind == NYA_ERROR_OUT_OF_MEMORY);

        // an escape that would not fit whole is refused rather than written half.
        char     small[3];
        NYA_Error short_encode = nya_percent_encode((const u8*)" ", 1, small, sizeof(small), &length);
        nya_assert(!short_encode.ok && short_encode.kind == NYA_ERROR_OUT_OF_MEMORY && small[0] == '\0');

        NYA_EXPECT(nya_percent_encode((const u8*)"", 0, small, sizeof(small), &length));
        nya_assert(length == 0 && small[0] == '\0');
    }

    printf("TEST: rendering refuses a buffer too small instead of cutting a url short\n");
    {
        NYA_Url url = { 0 };
        NYA_EXPECT(nya_url_parse("https://[::1]:443/x", 19, &url, nullptr));

        char small[10];
        u64  length = 0;

        NYA_Error result = nya_url_format(&url, small, sizeof(small), &length);
        nya_assert(!result.ok && result.kind == NYA_ERROR_OUT_OF_MEMORY && small[0] == '\0' && length == 0);
    }

    printf("PASSED: test_url (0 failures)\n");

    return EXIT_SUCCESS;
}
