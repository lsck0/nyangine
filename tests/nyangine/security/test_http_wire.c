/**
 * The http_server's wire, attacked over a real socket: path traversal into the static bundle, request
 * smuggling, and resource exhaustion. The other half of the pentest pass is test_http_security.c,
 * which reaches the router in-process; these three are about bytes and connections, so they need a port.
 *
 * Every refusal case is sent with a complete, valid request glued on behind it that asks for
 * /smuggled. The server has to answer exactly once and close: a second answer, or a single hit on the
 * /smuggled handler, is the stream having been framed two ways.
 *
 * The slowloris case waits out NYA_HTTP_IDLE_TIMEOUT_MS on the real clock, which is the one sleep of
 * any length here: the server reads the monotonic clock directly and there is nothing to pin.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

// After the engine: base_basic.h asks for POSIX 2008, which these only honour when they see it first.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/** A whole, valid request. Behind a refused one it must never run; see the file note. */
#define SMUGGLE "GET /smuggled HTTP/1.1\r\nHost: x\r\n\r\n"

/** How long one refusal may take to answer and close on loopback before the case counts as hung. */
#define ANSWER_WAIT_MS 1500

/** How long a request the server must not answer is watched for an answer. */
#define SILENCE_WAIT_MS 200

/** Slack past NYA_HTTP_IDLE_TIMEOUT_MS for a drip to be dropped: one tick of the loop below plus scheduling. */
#define TIMEOUT_SLACK_MS 1000

/** How often the slowloris peers send their next byte. Well under the timeout, which is the attack. */
#define DRIP_INTERVAL_MS 250

/** What the slow reader takes per DRIP_INTERVAL_MS: steady progress, far below what its answers need. */
#define SLOW_READ_BYTES 512

/** Answers the slow reader asks for at once; their sum stays under NYA_HTTP_MAX_PENDING_WRITE_BYTES so the bound is not what drops it. */
#define SLOW_READ_REQUESTS 7

/** Sockets the flood opens, four times the table, all from one address. */
#define FLOOD_COUNT (NYA_HTTP_MAX_CONNECTIONS * 4)

static char SCRATCH[512] = { 0 };

static u32 SMUGGLED_HITS = 0;

static const u8 PAGE_BYTES[]   = "<!doctype html><title>PAGE-BYTES</title>";
static const u8 SECRET_BYTES[] = "<!doctype html><title>TOP-SECRET-BYTES</title>";

/* ROUTES */

static NYA_HttpStatus route_a(NYA_HttpExchange* exchange) {
    return nya_http_response_text(exchange->response, "a", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static NYA_HttpStatus route_smuggled(NYA_HttpExchange* exchange) {
    SMUGGLED_HITS++;
    return nya_http_response_text(exchange->response, "SMUGGLED", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Answers the body it was framed as, so a test sees exactly where the parser put the boundary. */
static NYA_HttpStatus route_echo(NYA_HttpExchange* exchange) {
    const NYA_HttpRequest* request = exchange->request;
    return nya_http_response_bytes(exchange->response, request->body, request->body_size, NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK
                                                                                                                  : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Near the response cap, so a few pipelined answers outgrow what the kernel holds on the peer's behalf. */
static NYA_HttpStatus route_large(NYA_HttpExchange* exchange) {
    static u8 LARGE[NYA_HTTP_MAX_RESPONSE_BYTES / 2] = { 0 };
    return nya_http_response_bytes(exchange->response, LARGE, sizeof(LARGE), NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static const NYA_HttpRoute WIRE_ROUTES[] = {
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = "/a",
     .handler  = route_a,
     .summary  = "The request a pipeline is allowed to reach",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR }                            },
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = "/smuggled",
     .handler  = route_smuggled,
     .summary  = "The request behind every refusal, which must never run",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR }                            },
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = "/large",
     .handler  = route_large,
     .summary  = "An answer a slow reader cannot take in time",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR }                            },
    { .method   = NYA_HTTP_METHOD_POST,
     .path     = "/echo",
     .handler  = route_echo,
     .summary  = "Answers the body as it was framed",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

static const NYA_HttpRouter WIRE_ROUTER = {
    .name        = "wire",
    .routes      = WIRE_ROUTES,
    .route_count = nya_carray_length(WIRE_ROUTES),
};

/* THE CLIENT */

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

static u64 elapsed_ms(u64 since_ns) {
    return (nya_clock_get_monotonic_ns() - since_ns) / 1000000ULL;
}

/** Starts on a port the system picked; the limits are the caller's, so each suite states the ones it tests. */
static u16 start_server(NYA_HttpConfig config) {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    config.port = port;
    NYA_EXPECT(nya_system_http_init(config), "while starting the test server");
    NYA_EXPECT(nya_http_server_merge(&WIRE_ROUTER));

    return port;
}

/** Wide open, so the suites that are not about the limits never trip them. */
static const NYA_HttpConfig UNLIMITED = {
    .max_connections_per_address = NYA_HTTP_MAX_CONNECTIONS,
    .requests_per_second         = 100000,
    .request_burst               = 100000,
};

static NYA_OsSocket connect_to(u16 port) {
    NYA_OsAddress address = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

    NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);
    nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

    NYA_OsSocketWait watched = { .socket = socket, .writable = true };
    u32              ready   = 0;
    nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
    nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

    return socket;
}

static void send_all(NYA_OsSocket socket, const u8* data, u64 size) {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(socket, data, size, &wrote) == NYA_OS_SOCKET_OK && wrote == size);
}

/** Ticks and reads until the server closes or `wait_ms` passes. True when it closed. */
static b8 drain(NYA_OsSocket socket, u32 wait_ms, OUT char* buffer, u64 capacity, OUT u64* out_filled) {
    u64 started = nya_clock_get_monotonic_ns();
    u64 filled  = 0;
    b8  closed  = false;

    while (elapsed_ms(started) < wait_ms && filled + 1 < capacity) {
        nya_system_http_tick();

        u64                read    = 0;
        NYA_OsSocketStatus status  = nya_os_socket_receive(socket, (u8*)(buffer + filled), capacity - filled - 1, &read);
        filled                    += read;

        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) {
            closed = true;
            break;
        }

        if (read == 0) sleep_ms(1);
    }

    buffer[filled] = '\0';
    *out_filled    = filled;

    return closed;
}

/** Gives the server the ticks it needs to notice a closed peer and hand its slot back. */
static void settle(void) {
    for (u32 attempt = 0; attempt < 8; attempt++) {
        nya_system_http_tick();
        sleep_ms(1);
    }
}

/** How many answers came back, counted by status line. No body in this file contains the spelling. */
static u32 answers_in(NYA_ConstCString text) {
    u32 count = 0;
    for (const char* at = strstr(text, "HTTP/1.1 "); at != nullptr; at = strstr(at + 1, "HTTP/1.1 ")) count++;
    return count;
}

static u32 status_of(NYA_ConstCString answer) {
    if (strncmp(answer, "HTTP/1.1 ", 9) != 0) return 0;
    return (u32)strtoul(answer + 9, nullptr, 10);
}

/** One connection, `size` raw bytes, and everything that came back until the server closed it. */
typedef struct {
    char text[NYA_HTTP_MAX_RESPONSE_BYTES];
    u64  size;
    b8   closed;
} Answer;

static void converse(u16 port, const u8* data, u64 size, u32 wait_ms, OUT Answer* out) {
    NYA_OsSocket socket = connect_to(port);
    send_all(socket, data, size);

    out->closed = drain(socket, wait_ms, out->text, sizeof(out->text), &out->size);

    nya_os_socket_close(socket);
    settle();
}

/**
 * One request that must be refused: `request` with SMUGGLE behind it, answered once with `expected`
 * (zero is any 4xx), closed, and the smuggled request never run.
 * */
typedef struct {
    NYA_ConstCString request;
    u32              expected;
    NYA_ConstCString why;
} Refusal;

static void check_refused(u16 port, NYA_Arena* arena, const Refusal* refusal) {
    NYA_String* bytes = nya_string_sprintf(arena, "%s%s", refusal->request, SMUGGLE);

    static Answer answer = { 0 };
    u32           before = SMUGGLED_HITS;

    converse(port, (const u8*)bytes->items, bytes->length, ANSWER_WAIT_MS, &answer);

    u32 status = status_of(answer.text);

    if (refusal->expected != 0) {
        nya_check(status == refusal->expected, "%s: expected %u, got %u", refusal->why, refusal->expected, status);
    } else {
        nya_check(status >= 400 && status < 500, "%s: expected a 4xx, got %u", refusal->why, status);
    }

    nya_check(answer.closed, "%s: the connection was left open after a refusal", refusal->why);
    nya_check(answers_in(answer.text) == 1, "%s: %u answers to one refused stream", refusal->why, answers_in(answer.text));
    nya_check(SMUGGLED_HITS == before, "%s: the request behind it ran", refusal->why);
}

/* PATH TRAVERSAL */

static NYA_CString scratch_path(NYA_Arena* arena, NYA_ConstCString name) {
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", SCRATCH, name));
}

static void test_path_traversal(NYA_Arena* arena) {
    NYA_String* temp = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(arena, &temp));
    (void)snprintf(SCRATCH, sizeof(SCRATCH), "%.*s/nyangine_http_wire_test", (int)temp->length, temp->items);

    // a leftover from an interrupted run is only something in the way.
    (void)nya_filesystem_delete_recursive(SCRATCH);
    NYA_EXPECT(nya_filesystem_create_directory(SCRATCH));
    defer(void) nya_filesystem_delete_recursive(SCRATCH);

    /* The bundle's root is web/, the secret sits beside it, and web/linked is a directory symlink out to where another secret lives. */
    NYA_CString root = scratch_path(arena, "web");
    NYA_EXPECT(nya_filesystem_create_directory(root));
    NYA_EXPECT(nya_filesystem_create_directory(scratch_path(arena, "outside")));
    NYA_EXPECT(nya_file_write(scratch_path(arena, "web/page.html"), (NYA_ConstCString)PAGE_BYTES));
    NYA_EXPECT(nya_file_write(scratch_path(arena, "secret.html"), (NYA_ConstCString)SECRET_BYTES));
    NYA_EXPECT(nya_file_write(scratch_path(arena, "outside/leak.html"), (NYA_ConstCString)SECRET_BYTES));
    nya_assert(nya_os_file_link_set(scratch_path(arena, "web/linked"), scratch_path(arena, "outside")) == NYA_OS_FILE_STATUS_OK);

    // TEST: a handle that reaches outside the root through a symlinked directory is refused at mount, like a symlinked file.
    {
        NYA_HttpStaticFile escape = {
            .asset = scratch_path(arena, "web/linked/leak.html"),
            .path  = "/leak.html",
            .data  = SECRET_BYTES,
            .size  = sizeof(SECRET_BYTES) - 1,
        };

        NYA_Error mounted = nya_http_static_mount((NYA_HttpStaticConfig){ .files = &escape, .count = 1, .root = root });
        nya_check(!mounted.ok, "a handle through a directory symlink out of the root was mounted");
        nya_check(nya_http_static_file_count() == 0, "and something of it was left behind");
        nya_http_static_unmount();
    }

    NYA_HttpStaticFile page = {
        .asset = scratch_path(arena, "web/page.html"),
        .path  = "/page.html",
        .data  = PAGE_BYTES,
        .size  = sizeof(PAGE_BYTES) - 1,
    };

    NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = &page, .count = 1, .root = root }));
    defer nya_http_static_unmount();

    u16   port = start_server(UNLIMITED);
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(nya_http_static_router()));

    NYA_ConstCString hashed = nya_http_static_url(page.asset);
    nya_assert(hashed != nullptr);

    static Answer answer = { 0 };

    // TEST: the control. Both names serve the page, so a refusal below is a refusal and not a server that serves nothing.
    {
        NYA_ConstCString plain = "GET /page.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        converse(port, (const u8*)plain, strlen(plain), ANSWER_WAIT_MS, &answer);
        nya_check(status_of(answer.text) == 200 && strstr(answer.text, "PAGE-BYTES") != nullptr, "the page is served by name");

        NYA_String* by_hash = nya_string_sprintf(arena, "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", hashed);
        converse(port, (const u8*)by_hash->items, by_hash->length, ANSWER_WAIT_MS, &answer);
        nya_check(status_of(answer.text) == 200 && strstr(answer.text, "PAGE-BYTES") != nullptr, "the page is served by hash");
    }

    // TEST: every spelling of "somewhere else" is a 400 or a 404, and never a byte of either file.
    {
        NYA_ConstCString TARGETS[] = {
            "/../secret.html",                 // the plain climb
            "/page.html/../../secret.html",    // through a file that exists
            "/static/../secret.html",          // through the prefix
            "/static/../../../../etc/passwd",  // and all the way up
            "/%2e%2e/secret.html",             // percent encoded
            "/%2E%2E/secret.html",             // in upper case
            "/.%2e/secret.html",               // half encoded
            "/%2e./secret.html",               // the other half
            "/%2e%2e%2fsecret.html",           // with an encoded separator
            "/..%2Fsecret.html",               //
            "/static%2f..%2f..%2fsecret.html", //
            "/%252e%252e%252fsecret.html",     // double encoded, decoded exactly once
            "/%252e%252e/secret.html",         //
            "/..%5csecret.html",               // an encoded backslash
            "/%5c..%5csecret.html",            //
            "/..\\secret.html",                // a raw backslash
            "/page.html%00",                   // an encoded NUL
            "/page.html%00.css",               // and one that would truncate a suffix check
            "/%c0%ae%c0%ae/secret.html",       // an overlong UTF-8 dot
            "/..%c0%afsecret.html",            // an overlong UTF-8 separator
            "/%e0%80%ae%e0%80%ae/secret.html", // a three byte overlong dot
            "/secret.html",                    // a file that exists, but not in the bundle
            "/etc/passwd",                     // an absolute path
            "//etc/passwd",                    // one that reads as an authority
            "/static//etc/passwd",             // an empty segment
            "/C:/secret.html",                 // a drive letter
            "C:\\secret.html",                 // and a whole Windows path
            "../secret.html",                  // a relative target
            "file:///etc/passwd",              // another scheme
            "http://x/../secret.html",         // the absolute form, which is for proxies
            "/./page.html",                    // a dot segment, even a harmless one
        };

        for (u64 index = 0; index < nya_carray_length(TARGETS); index++) {
            NYA_String* request = nya_string_sprintf(arena, "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", TARGETS[index]);
            converse(port, (const u8*)request->items, request->length, ANSWER_WAIT_MS, &answer);

            u32 status = status_of(answer.text);
            nya_check(status == 400 || status == 404, "'%s' answered %u", TARGETS[index], status);
            nya_check(strstr(answer.text, "TOP-SECRET") == nullptr, "'%s' served the secret", TARGETS[index]);
            nya_check(strstr(answer.text, "PAGE-BYTES") == nullptr, "'%s' served the page", TARGETS[index]);
        }

        // the same climbs under the hashed name, which is the one route with a hash in it.
        NYA_ConstCString SUFFIXES[] = { "/../../secret.html", "%2f..%2f..%2fsecret.html", "/.", "%00", "/" };

        for (u64 index = 0; index < nya_carray_length(SUFFIXES); index++) {
            NYA_String* request = nya_string_sprintf(arena, "GET %s%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", hashed, SUFFIXES[index]);
            converse(port, (const u8*)request->items, request->length, ANSWER_WAIT_MS, &answer);

            u32 status = status_of(answer.text);
            nya_check(status == 400 || status == 404, "the hashed name plus '%s' answered %u", SUFFIXES[index], status);
            nya_check(strstr(answer.text, "TOP-SECRET") == nullptr, "the hashed name plus '%s' served the secret", SUFFIXES[index]);
        }
    }

    // TEST: a raw NUL in the target is not a terminator anybody gets to act on.
    {
        static const u8 RAW[] = "GET /page.html\0/../secret.html HTTP/1.1\r\nHost: x\r\n\r\n";
        converse(port, RAW, sizeof(RAW) - 1, ANSWER_WAIT_MS, &answer);

        nya_check(status_of(answer.text) == 400, "a raw NUL in the target answered %u", status_of(answer.text));
        nya_check(strstr(answer.text, "PAGE-BYTES") == nullptr, "and served the part before it");
    }
}

/* REQUEST SMUGGLING */

static void test_smuggling(NYA_Arena* arena) {
    u16   port = start_server(UNLIMITED);
    defer nya_system_http_deinit();

    // TEST: every ambiguous framing is refused outright, answered once, and nothing behind it runs.
    {
        static const Refusal REFUSALS[] = {
            // Content-Length against Transfer-Encoding, in both orders: the classic CL.TE and TE.CL.
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",           400, "CL then TE"                           },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",           400, "TE then CL"                           },

            // an obfuscated Transfer-Encoding a front end might read as chunked and this as nothing.
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: xchunked\r\n\r\n",                                        501, "TE: xchunked"                         },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, identity\r\n\r\n",                               501, "TE: a list"                           },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: identity, chunked\r\n\r\n",                               501, "TE: chunked last in a list"           },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding : chunked\r\n\r\n0\r\n\r\n",                               400, "TE with a space before the colon"     },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\x0b"
              "chunked\r\n\r\n0\r\n\r\n",                                                                        400,
             "TE with a vertical tab"                                                                                                                                     },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",  400, "TE twice"                             },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n\r\n0\r\n\r\n",
             501,                                                                                                                  "TE twice, differing"                  },
            { "POST /echo HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",      400, "TE on HTTP/1.0"                       },

            // Content-Length that two readers could read two ways.
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 5\r\n\r\nhello",                        400, "CL twice, differing"                  },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello",                        400, "CL twice, the same"                   },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 5\r\n\r\nhello",                                          400, "CL as a list"                         },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\nhello",                                            400, "CL with a sign"                       },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n",                                                 400, "CL negative"                          },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0x5\r\n\r\nhello",                                           400, "CL in hex"                            },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5 0\r\n\r\nhello",                                           400, "CL with a space inside"               },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n",                                                    400, "CL empty"                             },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551621\r\n\r\n",                               400, "CL that wraps a u64"                  },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\nhello",                                            400, "CL with a space before the colon"     },

            // a body on a verb that has none, which one reader counts as a body and another as the next request.
            { "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 35\r\n\r\n",                                                     400, "CL on a GET, the smuggle as its body" },
            { "GET /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",                                    400, "chunked on a GET"                     },

            // chunked, at its edges.
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3;a\nb\r\nabc\r\n0\r\n\r\n",
             400,                                                                                                                  "a bare LF in a chunk extension"       },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3;a\rb\r\nabc\r\n0\r\n\r\n",
             400,                                                                                                                  "a bare CR in a chunk extension"       },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX: a\nb\r\n\r\n",         400, "a bare LF in a trailer"               },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcd\r\n0\r\n\r\n",                   400, "a chunk longer than its size"         },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc\r\n0\r\n\r\n",                    400, "a chunk shorter than its size"        },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0x3\r\nabc\r\n0\r\n\r\n",                  0,   "a chunk size with 0x"                 },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n-3\r\nabc\r\n0\r\n\r\n",                   0,   "a negative chunk size"                },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n 3\r\nabc\r\n0\r\n\r\n",                   0,   "a chunk size with a space"            },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n\r\nabc\r\n0\r\n\r\n",                     400, "an empty chunk size"                  },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffff\r\n",                    413, "a chunk size that wraps"              },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\nabc\n0\n\n",                            400, "chunked with bare LF lines"           },

            // line endings a front end might accept and this must not guess about.
            { "GET /a HTTP/1.1\nHost: x\r\n\r\n",                                                                             400, "a bare LF after the request line"     },
            { "GET /a HTTP/1.1\r\nHost: x\nX-Hidden: y\r\n\r\n",                                                              400, "a bare LF inside the headers"         },
            { "GET /a HTTP/1.1\r\nHost: x\rX-Hidden: y\r\n\r\n",                                                              400, "a bare CR inside the headers"         },
            { "GET /a HTTP/1.1\r\nHost: x\r\n\n",                                                                             400, "a bare LF where the blank line goes"  },

            // obs-fold, which RFC 9112 lets a server refuse and which refusing is the only reading of that does not guess.
            { "GET /a HTTP/1.1\r\nHost: x\r\nX-Folded: a\r\n b\r\n\r\n",                                                      400, "obs-fold with a space"                },
            { "GET /a HTTP/1.1\r\nHost: x\r\nX-Folded: a\r\n\tb\r\n\r\n",                                                     400, "obs-fold with a tab"                  },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n 5\r\n\r\nhello",                                         400, "a folded Content-Length"              },
            { "POST /echo HTTP/1.1\r\nHost: x\r\nX-A: b\r\n Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",                     400, "a folded Transfer-Encoding"           },

            // the request line, which has exactly two single spaces in it.
            { "GET  /a HTTP/1.1\r\nHost: x\r\n\r\n",                                                                          400, "two spaces after the method"          },
            { "GET /a  HTTP/1.1\r\nHost: x\r\n\r\n",                                                                          400, "two spaces after the target"          },
            { " GET /a HTTP/1.1\r\nHost: x\r\n\r\n",                                                                          400, "a leading space"                      },
            { "GET /a HTTP/1.1 \r\nHost: x\r\n\r\n",                                                                          400, "a trailing space"                     },
            { "GET\t/a\tHTTP/1.1\r\nHost: x\r\n\r\n",                                                                         400, "tabs for spaces"                      },
            { "GET /a HTTP/1.1 x\r\nHost: x\r\n\r\n",                                                                         400, "a fourth part"                        },
            { "GET /a b HTTP/1.1\r\nHost: x\r\n\r\n",                                                                         400, "a space inside the target"            },
            { "GET /a\r\nHost: x\r\n\r\n",                                                                                    400, "an HTTP/0.9 request line"             },
            { "\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n",                                                                       400, "an empty line before the request"     },
            { "GET /a HTTP/2.0\r\nHost: x\r\n\r\n",                                                                           505, "a version this is not"                },
            { "GET /a HTTP/1.10\r\nHost: x\r\n\r\n",                                                                          505, "a version with a trailing digit"      },
            { "GET /a http/1.1\r\nHost: x\r\n\r\n",                                                                           505, "a version in lower case"              },
            { "get /a HTTP/1.1\r\nHost: x\r\n\r\n",                                                                           501, "a method in lower case"               },

            // header names and Host, which decide what the request is and who it is for.
            { "GET /a HTTP/1.1\r\nHost: x\r\nX Y: z\r\n\r\n",                                                                 400, "a space inside a header name"         },
            { "GET /a HTTP/1.1\r\nHost: x\r\n: z\r\n\r\n",                                                                    400, "an empty header name"                 },
            { "GET /a HTTP/1.1\r\nHost: x\r\nX-No-Colon\r\n\r\n",                                                             400, "a header with no colon"               },
            { "GET /a HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",                                                                400, "Host twice"                           },
            { "GET /a HTTP/1.1\r\n\r\n",                                                                                      400, "no Host on HTTP/1.1"                  },
        };

        for (u64 index = 0; index < nya_carray_length(REFUSALS); index++) check_refused(port, arena, &REFUSALS[index]);
    }

    // TEST: a NUL in a header name or value is refused, sent raw.
    {
        static const u8 NUL_NAME[]  = "GET /a HTTP/1.1\r\nHost: x\r\nX-A\0B: c\r\n\r\n" SMUGGLE;
        static const u8 NUL_VALUE[] = "GET /a HTTP/1.1\r\nHost: x\r\nX-A: b\0c\r\n\r\n" SMUGGLE;

        static Answer answer = { 0 };
        u32           before = SMUGGLED_HITS;

        converse(port, NUL_NAME, sizeof(NUL_NAME) - 1, ANSWER_WAIT_MS, &answer);
        nya_check(status_of(answer.text) == 400 && answer.closed, "a NUL in a header name answered %u", status_of(answer.text));

        converse(port, NUL_VALUE, sizeof(NUL_VALUE) - 1, ANSWER_WAIT_MS, &answer);
        nya_check(status_of(answer.text) == 400 && answer.closed, "a NUL in a header value answered %u", status_of(answer.text));

        nya_check(SMUGGLED_HITS == before, "a request behind a NUL ran");
    }

    // TEST: a request with bare LF line endings throughout is never answered as a request, and nothing inside it runs.
    {
        static Answer    answer = { 0 };
        NYA_ConstCString bare   = "GET /smuggled HTTP/1.1\nHost: x\n\n";
        u32              before = SMUGGLED_HITS;

        converse(port, (const u8*)bare, strlen(bare), SILENCE_WAIT_MS, &answer);

        nya_check(status_of(answer.text) != 200, "a bare LF request was answered as one");
        nya_check(SMUGGLED_HITS == before, "a bare LF request ran");
    }

    // TEST: pipelining frames every request exactly where its own framing says, and a body that looks like a request stays a body.
    {
        static Answer answer = { 0 };
        u32           before = SMUGGLED_HITS;

        static const u8 BY_LENGTH[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: 35\r\n\r\n" SMUGGLE
                                      "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        static_assert(sizeof(SMUGGLE) - 1 == 35, "the Content-Length above is the length of SMUGGLE");

        converse(port, BY_LENGTH, sizeof(BY_LENGTH) - 1, ANSWER_WAIT_MS, &answer);

        nya_check(answers_in(answer.text) == 2, "a POST and a GET pipelined got %u answers", answers_in(answer.text));
        nya_check(strstr(answer.text, "GET /smuggled HTTP/1.1\r\n") != nullptr, "the body came back as the body");
        nya_check(strstr(answer.text, "\r\n\r\na") != nullptr && answer.closed, "and the GET behind it was answered, then closed");

        static const u8 BY_CHUNKS[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
                                      "10\r\nGET /smuggled HT\r\n13\r\nTP/1.1\r\nHost: x\r\n\r\n\r\n0\r\nX-Trailer: t\r\n\r\n"
                                      "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

        converse(port, BY_CHUNKS, sizeof(BY_CHUNKS) - 1, ANSWER_WAIT_MS, &answer);

        nya_check(answers_in(answer.text) == 2, "a chunked POST and a GET pipelined got %u answers", answers_in(answer.text));
        nya_check(strstr(answer.text, "GET /smuggled HTTP/1.1\r\nHost: x\r\n\r\n") != nullptr, "the chunks were joined into the body");

        static const u8 THREE[] =
            "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

        converse(port, THREE, sizeof(THREE) - 1, ANSWER_WAIT_MS, &answer);
        nya_check(answers_in(answer.text) == 3 && answer.closed, "three pipelined GETs got %u answers", answers_in(answer.text));

        nya_check(SMUGGLED_HITS == before, "a body was framed as a request");
    }

    // TEST: the control for the table above. The smuggled request on its own does run, so a zero there means refused and not unroutable.
    {
        static Answer answer = { 0 };
        u32           before = SMUGGLED_HITS;

        converse(port, (const u8*)SMUGGLE, strlen(SMUGGLE), 50, &answer);
        nya_check(SMUGGLED_HITS == before + 1 && status_of(answer.text) == 200, "the smuggled request is a real route");
    }
}

/* RESOURCE EXHAUSTION */

/** A POSIX socket from 127.0.0.2, which the per address limits count as another machine. */
static s32 connect_from_other_address(u16 port) {
    s32 descriptor = socket(AF_INET, SOCK_STREAM, 0);
    nya_assert(descriptor >= 0);

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = 0 };
    nya_assert(inet_pton(AF_INET, "127.0.0.2", &local.sin_addr) == 1);
    nya_assert(bind(descriptor, (const struct sockaddr*)&local, sizeof(local)) == 0);

    struct sockaddr_in remote = { .sin_family = AF_INET, .sin_port = htons(port) };
    nya_assert(inet_pton(AF_INET, "127.0.0.1", &remote.sin_addr) == 1);
    nya_assert(connect(descriptor, (const struct sockaddr*)&remote, sizeof(remote)) == 0);

    return descriptor;
}

static void test_exhaustion(NYA_Arena* arena) {
    static Answer answer = { 0 };

    // TEST: oversized requests are refused with the status that names what was too big, and closed.
    {
        u16   port = start_server(UNLIMITED);
        defer nya_system_http_deinit();

        NYA_String* long_target = nya_string_create(arena);
        nya_string_extend(long_target, "GET /");
        for (u32 index = 0; index < NYA_URL_MAX_BYTES + 16; index++) nya_string_extend(long_target, "a");
        nya_string_extend(long_target, " HTTP/1.1\r\nHost: x\r\n\r\n");

        NYA_String* huge_value = nya_string_create(arena);
        nya_string_extend(huge_value, "GET /a HTTP/1.1\r\nHost: x\r\nX-Big: ");
        for (u32 index = 0; index < NYA_HTTP_MAX_HEAD_BYTES + 16; index++) nya_string_extend(huge_value, "b");
        nya_string_extend(huge_value, "\r\n\r\n");

        NYA_String* many_headers = nya_string_create(arena);
        nya_string_extend(many_headers, "GET /a HTTP/1.1\r\nHost: x\r\n");
        for (u32 index = 0; index < NYA_HTTP_MAX_HEAD_BYTES / 4; index++) nya_string_extend(many_headers, "X:\r\n");
        nya_string_extend(many_headers, "\r\n");

        // no blank line, ever: the head bound is what ends it, not the peer.
        NYA_String* endless = nya_string_create(arena);
        for (u32 index = 0; index < NYA_HTTP_MAX_REQUEST_BYTES; index++) nya_string_extend(endless, "z");

        NYA_String* too_many_chunks = nya_string_create(arena);
        nya_string_extend(too_many_chunks, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
        for (u32 index = 0; index <= NYA_HTTP_MAX_CHUNKS; index++) nya_string_extend(too_many_chunks, "1\r\nq\r\n");
        nya_string_extend(too_many_chunks, "0\r\n\r\n");

        NYA_String* chunks_past_body =
            nya_string_sprintf(arena, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n", NYA_HTTP_MAX_BODY_BYTES + 1);

        NYA_String* length_past_body =
            nya_string_sprintf(arena, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", NYA_HTTP_MAX_BODY_BYTES + 1);

        NYA_String* trailers = nya_string_create(arena);
        nya_string_extend(trailers, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nq\r\n0\r\n");
        for (u32 index = 0; index <= NYA_HTTP_MAX_TRAILERS; index++) nya_string_extend(trailers, "X: t\r\n");
        nya_string_extend(trailers, "\r\n");

        const struct {
            NYA_String*      bytes;
            u32              expected;
            NYA_ConstCString why;
        } OVERSIZED[] = {
            { long_target,      414, "a target past NYA_URL_MAX_BYTES"        },
            { huge_value,       431, "a header past the head bound"           },
            { many_headers,     431, "headers enough to pass the head bound"  },
            { endless,          431, "a head that never ends"                 },
            { too_many_chunks,  413, "one chunk past NYA_HTTP_MAX_CHUNKS"     },
            { chunks_past_body, 413, "a chunk past NYA_HTTP_MAX_BODY_BYTES"   },
            { length_past_body, 413, "a Content-Length past the body bound"   },
            { trailers,         431, "one trailer past NYA_HTTP_MAX_TRAILERS" },
        };

        for (u64 index = 0; index < nya_carray_length(OVERSIZED); index++) {
            u32 before = SMUGGLED_HITS;

            NYA_String* bytes = nya_string_sprintf(arena, "%.*s%s", (int)OVERSIZED[index].bytes->length, OVERSIZED[index].bytes->items, SMUGGLE);
            converse(port, (const u8*)bytes->items, bytes->length, ANSWER_WAIT_MS, &answer);

            u32 status = status_of(answer.text);
            nya_check(status == OVERSIZED[index].expected, "%s: expected %u, got %u", OVERSIZED[index].why, OVERSIZED[index].expected, status);
            nya_check(answer.closed, "%s: left open", OVERSIZED[index].why);
            nya_check(SMUGGLED_HITS == before, "%s: the request behind it ran", OVERSIZED[index].why);
        }

        // the largest legal body is still answered, so the bound is where it says and not below it.
        NYA_String* largest =
            nya_string_sprintf(arena, "POST /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: %d\r\n\r\n", NYA_HTTP_MAX_BODY_BYTES);
        for (u32 index = 0; index < NYA_HTTP_MAX_BODY_BYTES; index++) nya_string_extend(largest, "e");

        converse(port, (const u8*)largest->items, largest->length, ANSWER_WAIT_MS, &answer);
        nya_check(status_of(answer.text) == 200, "the largest legal body answered %u", status_of(answer.text));
    }

    // TEST: headers past the table are dropped, not refused, and the framing headers after them still count.
    {
        u16   port = start_server(UNLIMITED);
        defer nya_system_http_deinit();

        NYA_String* padded = nya_string_create(arena);
        nya_string_extend(padded, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\n");
        for (u32 index = 0; index < NYA_HTTP_MAX_HEADERS + 8; index++) nya_string_extend_sprintf(padded, "X-Pad-%u: v\r\n", index);
        nya_string_extend(padded, "Content-Length: 35\r\n\r\n" SMUGGLE "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        u32 before = SMUGGLED_HITS;
        converse(port, (const u8*)padded->items, padded->length, ANSWER_WAIT_MS, &answer);

        nya_check(answers_in(answer.text) == 2, "a Content-Length behind the padding was missed: %u answers", answers_in(answer.text));
        nya_check(strstr(answer.text, "GET /smuggled HTTP/1.1\r\n") != nullptr, "and its body was not the body");
        nya_check(SMUGGLED_HITS == before, "the padding hid a request");

        // and the same trick with Transfer-Encoding, which must still meet the Content-Length it contradicts.
        NYA_String* conflicting = nya_string_create(arena);
        nya_string_extend(conflicting, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n");
        for (u32 index = 0; index < NYA_HTTP_MAX_HEADERS + 8; index++) nya_string_extend_sprintf(conflicting, "X-Pad-%u: v\r\n", index);
        nya_string_extend(conflicting, "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");

        Refusal refusal = { .request = nya_string_to_cstring(arena, conflicting), .expected = 400, .why = "TE behind the padding" };
        check_refused(port, arena, &refusal);
    }

    // TEST: slowloris. A head or a body dripped a byte at a time is dropped on the timeout from its first byte, not held while it drips, and nobody
    // else waits on it.
    {
        u16   port = start_server(UNLIMITED);
        defer nya_system_http_deinit();

        NYA_OsSocket head = connect_to(port);
        NYA_OsSocket body = connect_to(port);
        defer {
            nya_os_socket_close(head);
            nya_os_socket_close(body);
        }

        NYA_ConstCString head_start = "GET /a HTTP/1.1\r\nHost: x\r\nX-Drip: ";
        NYA_ConstCString body_start = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4000\r\n\r\n";

        send_all(head, (const u8*)head_start, strlen(head_start));
        send_all(body, (const u8*)body_start, strlen(body_start));

        u64 started   = nya_clock_get_monotonic_ns();
        u64 dripped   = started;
        b8  head_open = true;
        b8  body_open = true;
        b8  served    = false;

        u64 head_closed_ms = 0;
        u64 body_closed_ms = 0;

        while ((head_open || body_open) && elapsed_ms(started) < NYA_HTTP_IDLE_TIMEOUT_MS * 3) {
            nya_system_http_tick();

            if (elapsed_ms(dripped) >= DRIP_INTERVAL_MS) {
                dripped = nya_clock_get_monotonic_ns();

                u64 wrote = 0;
                if (head_open) (void)nya_os_socket_send(head, (const u8*)"d", 1, &wrote);
                if (body_open) (void)nya_os_socket_send(body, (const u8*)"d", 1, &wrote);
            }

            u8  scratch[64] = { 0 };
            u64 read        = 0;

            if (head_open && nya_os_socket_receive(head, scratch, sizeof(scratch), &read) == NYA_OS_SOCKET_CLOSED) {
                head_open      = false;
                head_closed_ms = elapsed_ms(started);
            }

            if (body_open && nya_os_socket_receive(body, scratch, sizeof(scratch), &read) == NYA_OS_SOCKET_CLOSED) {
                body_open      = false;
                body_closed_ms = elapsed_ms(started);
            }

            // halfway through the attack, a third peer is served as if nothing were happening.
            if (!served && elapsed_ms(started) >= NYA_HTTP_IDLE_TIMEOUT_MS / 2) {
                served = true;

                NYA_ConstCString polite = "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
                converse(port, (const u8*)polite, strlen(polite), ANSWER_WAIT_MS, &answer);
                nya_check(status_of(answer.text) == 200, "a peer was kept waiting by the drips: %u", status_of(answer.text));
            }

            sleep_ms(2);
        }

        nya_check(!head_open, "a head dripped a byte every %d ms was held open for %d ms", DRIP_INTERVAL_MS, NYA_HTTP_IDLE_TIMEOUT_MS * 3);
        nya_check(!body_open, "a body dripped a byte every %d ms was held open for %d ms", DRIP_INTERVAL_MS, NYA_HTTP_IDLE_TIMEOUT_MS * 3);
        nya_check(
            head_open || head_closed_ms <= NYA_HTTP_IDLE_TIMEOUT_MS + TIMEOUT_SLACK_MS,
            "the dripped head lasted %llu ms",
            (unsigned long long)head_closed_ms
        );
        nya_check(
            body_open || body_closed_ms <= NYA_HTTP_IDLE_TIMEOUT_MS + TIMEOUT_SLACK_MS,
            "the dripped body lasted %llu ms",
            (unsigned long long)body_closed_ms
        );

        settle();
        nya_check(nya_http_server_connection_count() == 0, "a drip still holds a slot");
    }

    // TEST: slow reader. A peer taking its answers a little at a time has until the timeout to take all of them, not a fresh timeout per read.
    {
        u16   port = start_server(UNLIMITED);
        defer nya_system_http_deinit();

        // a small window and segment, set before the handshake, so the kernel buffers little on the reader's behalf and each read reopens the window.
        s32 descriptor = socket(AF_INET, SOCK_STREAM, 0);
        nya_assert(descriptor >= 0);

        int window  = 4096;
        int segment = 536;
        nya_assert(setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &window, sizeof(window)) == 0);
        nya_assert(setsockopt(descriptor, IPPROTO_TCP, TCP_MAXSEG, &segment, sizeof(segment)) == 0);

        struct sockaddr_in loopback = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        nya_assert(connect(descriptor, (struct sockaddr*)&loopback, sizeof(loopback)) == 0);
        nya_assert(fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK) == 0);

        NYA_OsSocket reader = { .handle = (u64)descriptor + 1 };
        defer nya_os_socket_close(reader);

        static_assert(SLOW_READ_REQUESTS * NYA_HTTP_MAX_RESPONSE_BYTES / 2 < NYA_HTTP_MAX_PENDING_WRITE_BYTES);
        NYA_ConstCString large = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
        for (u32 index = 0; index < SLOW_READ_REQUESTS; index++) send_all(reader, (const u8*)large, strlen(large));

        u64 started   = nya_clock_get_monotonic_ns();
        u64 read_at   = started;
        u64 took      = 0;
        u64 closed_ms = 0;

        while (closed_ms == 0 && elapsed_ms(started) < NYA_HTTP_IDLE_TIMEOUT_MS * 3) {
            nya_system_http_tick();

            if (elapsed_ms(read_at) >= DRIP_INTERVAL_MS) {
                read_at = nya_clock_get_monotonic_ns();

                u8  scratch[SLOW_READ_BYTES] = { 0 };
                u64 read                     = 0;
                if (nya_os_socket_receive(reader, scratch, sizeof(scratch), &read) == NYA_OS_SOCKET_OK) took += read;
            }

            if (nya_http_server_connection_count() == 0) closed_ms = elapsed_ms(started);
            sleep_ms(2);
        }

        nya_check(took > 0, "the slow reader never got a byte, so the case proved nothing");
        nya_check(closed_ms != 0, "a reader taking %d bytes every %d ms was held for %d ms", SLOW_READ_BYTES, DRIP_INTERVAL_MS, NYA_HTTP_IDLE_TIMEOUT_MS * 3);
        nya_check(closed_ms <= NYA_HTTP_IDLE_TIMEOUT_MS + TIMEOUT_SLACK_MS, "the slow reader lasted %llu ms", (unsigned long long)closed_ms);
    }

    // TEST: a connection flood from one address takes its share of the table and no more, and another address is still served.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_OsSocket flood[FLOOD_COUNT] = { 0 };
        for (u32 index = 0; index < FLOOD_COUNT; index++) flood[index] = connect_to(port);
        defer {
            for (u32 index = 0; index < FLOOD_COUNT; index++) nya_os_socket_close(flood[index]);
        }

        u32 most = 0;
        for (u32 attempt = 0; attempt < FLOOD_COUNT; attempt++) {
            nya_system_http_tick();
            most = nya_max(most, nya_http_server_connection_count());
            sleep_ms(1);
        }

        nya_check(
            most == NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS,
            "one address held %u connections, the cap is %d",
            most,
            NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS
        );

        // the rest were closed at accept, which the flood sees as an end of stream rather than a queue.
        u32 refused = 0;
        for (u32 index = 0; index < FLOOD_COUNT; index++) {
            u8  scratch[16] = { 0 };
            u64 read        = 0;
            if (nya_os_socket_receive(flood[index], scratch, sizeof(scratch), &read) == NYA_OS_SOCKET_CLOSED) refused++;
        }
        nya_check(
            refused == FLOOD_COUNT - NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS,
            "%u of the flood were closed, expected %d",
            refused,
            FLOOD_COUNT - NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS
        );

        s32   other = connect_from_other_address(port);
        defer close(other);

        NYA_ConstCString polite = "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        nya_assert(send(other, polite, strlen(polite), MSG_NOSIGNAL) == (ssize_t)strlen(polite));

        u64 filled  = 0;
        u64 started = nya_clock_get_monotonic_ns();

        while (elapsed_ms(started) < ANSWER_WAIT_MS && filled + 1 < sizeof(answer.text)) {
            nya_system_http_tick();

            ssize_t read = recv(other, answer.text + filled, sizeof(answer.text) - filled - 1, MSG_DONTWAIT);
            if (read == 0) break;
            if (read > 0) filled += (u64)read;

            sleep_ms(1);
        }

        answer.text[filled] = '\0';
        nya_check(status_of(answer.text) == 200, "another address was not served during a flood: %u", status_of(answer.text));
    }
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_http_wire");
    defer      nya_arena_destroy(arena);

    test_path_traversal(arena);
    test_smuggling(arena);
    test_exhaustion(arena);

    if (nya_check_failures() == 0) printf("PASSED: http wire\n");

    return nya_check_failures() == 0 ? 0 : 1;
}
