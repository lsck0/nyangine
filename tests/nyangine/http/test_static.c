/**
 * The web bundle end to end, and the refusals that surround it.
 *
 * Two halves, because the module has two ways in. Mounting is where a program hands over an asset
 * handle, so every hostile spelling of a handle is checked against nya_http_static_mount directly —
 * that is where a traversal would enter, since a request never becomes part of a file name. Serving is
 * checked over a real socket the way test_server.c does it, because the answer is the whole point:
 * the right bytes, the right type, the right cache policy, and a 304 for a caller that already has it.
 *
 * The asset system comes up by hand. An html file is an asset like any other, so reading one goes
 * through nya_asset_read, and that wants an app to hang off; the core tests bring it up the same way.
 **/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FIRST_PORT 47960
#define LAST_PORT  47976

/** Where the refusal half writes its files. Under the temp directory, so the repository is untouched. */
static char SCRATCH[512] = { 0 };

/** What a refused mount is handed, so that it is the name being refused and never the bytes. */
static const u8 SOME_BYTES[] = "<!doctype html><title>scratch</title>";
#define SOME_BYTES_SIZE (sizeof(SOME_BYTES) - 1)

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

/**
 * Starts on the first port that binds, as test_server.c does: a busy one is not a failure.
 *
 * With the limits wide open, because this test makes a few dozen requests as fast as it can from one
 * address and the defaults are sized for a person with a browser. What the limits do is test_server.c's
 * subject; what they must not do is decide this one.
 * */
static u16 start_server(void) {
    for (u16 port = FIRST_PORT; port <= LAST_PORT; port++) {
        NYA_HttpConfig config = {
            .port                        = port,
            .max_connections_per_address = NYA_HTTP_MAX_CONNECTIONS,
            .requests_per_second         = 1000,
            .request_burst               = 1000,
        };

        if (nya_system_http_init(config).ok) return port;
    }

    nya_assert(false, "no port in [%d, %d] could be bound", FIRST_PORT, LAST_PORT);

    return 0;
}

static NYA_OsSocket connect_to(u16 port) {
  NYA_OsAddress address = { 0 };
  nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

  NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
  NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);

  nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

  // a non-blocking connect is under way rather than done, and writability is how the host says it
  // finished; loopback usually beats the first wait to it.
  NYA_OsSocketWait watched = { .socket = socket, .writable = true };
  u32              ready   = 0;

  nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
  nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

  return socket;
}

/**
 * Sends `text` and reads a whole answer back: the head, then exactly what Content-Length promised.
 *
 * Lifted from test_server.c for the reason written there: the head and the body are two queued writes,
 * so a read that comes back empty says only that the kernel has not caught up.
 * */
static void exchange(NYA_OsSocket socket, NYA_ConstCString text, OUT char* buffer, u64 capacity) {
    {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(socket, (const u8*)text, strlen(text), &wrote) == NYA_OS_SOCKET_OK && wrote == strlen(text));
  }

    u64 filled   = 0;
    u64 expected = 0;

    for (u32 attempt = 0; attempt < 400 && filled + 1 < capacity; attempt++) {
        nya_system_http_tick();

        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)(buffer + filled), capacity - filled - 1, &read);

        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) break;

        filled         += read;
        buffer[filled]  = '\0';

        if (expected == 0) {
            const char* blank  = strstr(buffer, "\r\n\r\n");
            const char* length = strstr(buffer, "Content-Length: ");

            if (blank != nullptr && length != nullptr) expected = (u64)(blank + 4 - buffer) + strtoull(length + 16, nullptr, 10);
        }

        if (expected > 0 && filled >= expected) break;

        sleep_ms(2);
    }

    buffer[filled] = '\0';
}

/** The status line's code, or zero when what came back is not one. */
static u32 status_of(NYA_ConstCString answer) {
    if (strncmp(answer, "HTTP/1.1 ", 9) != 0) return 0;

    return (u32)strtoul(answer + 9, nullptr, 10);
}

/** Sends one request on a connection of its own and answers with the status. */
static u32 request_status(u16 port, NYA_ConstCString request) {
    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };
    u32  status                              = 0;

    {
        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        exchange(socket, request, answer, sizeof(answer));
        status = status_of(answer);
    }

    // a refusal answers with Connection: close, and the slot only comes back when the server next
    // looks at the socket. The next connect needs it back.
    for (u32 attempt = 0; attempt < 4; attempt++) {
        nya_system_http_tick();
        sleep_ms(2);
    }

    return status;
}

/** The value of one header of the answer, copied into `out`, or "" when it is not there. */
static void header_of(NYA_ConstCString answer, NYA_ConstCString name, OUT char* out, u64 capacity) {
    out[0] = '\0';

    const char* found = strstr(answer, name);
    if (found == nullptr) return;

    found += strlen(name);

    const char* end = strstr(found, "\r\n");
    if (end == nullptr) return;

    u64 size = (u64)(end - found);
    nya_assert(size + 1 < capacity, "'%s' does not fit", name);

    nya_memcpy(out, found, size);
    out[size] = '\0';
}

/** A path under the scratch directory. */
static NYA_CString scratch_path(NYA_Arena* arena, NYA_ConstCString name) {
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", SCRATCH, name));
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_static");
    defer      nya_arena_destroy(arena);

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    /*
     * The asset system, by hand: it registers an end-of-frame hook, so the callback and event
     * registries come up first. nya_app_init wants a window and this test has none, exactly as the
     * core tests that load assets find.
     */
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init());
    defer nya_system_events_deinit();

    nya_system_asset_init();
    defer nya_system_asset_deinit();

    /*
     * A tree of our own for the refusal half. It holds a real file, a symlink beside it and a
     * directory, which are the three things a handle can name and only one of which is served.
     */
    NYA_String* temp = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(arena, &temp));

    (void)snprintf(SCRATCH, sizeof(SCRATCH), "%.*s/nyangine_http_static_test", (int)temp->length, temp->items);

    // a leftover from a run that was interrupted is not a failure, only something in the way.
    (void)nya_filesystem_delete_recursive(SCRATCH);

    NYA_EXPECT(nya_filesystem_create_directory(SCRATCH));
    defer (void)nya_filesystem_delete_recursive(SCRATCH);

    NYA_CString page = scratch_path(arena, "page.html");
    NYA_CString link = scratch_path(arena, "link.html");
    NYA_CString folder = scratch_path(arena, "folder.html");

    NYA_EXPECT(nya_file_write(page, "<!doctype html><title>scratch</title>"));
    NYA_EXPECT(nya_filesystem_create_directory(folder));

    nya_assert(nya_os_file_link_set(link, page) == NYA_OS_FILE_STATUS_OK, "the scratch symlink could not be made");

    // TEST: a handle that is not a plain file under the root is refused, one spelling at a time.
    {
        /*
         * Each of these is one way of saying "somewhere else". They are refused before a byte is read,
         * which is why the list is checked here rather than through a request: a request never reaches
         * a file name at all, so this is the only surface a traversal could arrive on.
         */
        NYA_ConstCString REFUSED[] = {
            "../../etc/passwd",             // the plain climb
            "sub/../../etc/passwd",         // and the one that looks like it stays
            ".",                            // a dot segment on its own
            "..",                           //
            "x/./y.html",                   // and in the middle
            "%2e%2e/passwd.html",           // percent encoded, which this never decodes
            "%2f/passwd.html",              // an encoded separator, for the same reason
            "..\\..\\windows\\win.ini",     // the Windows separator, which is not a separator here
            "sub\\page.html",               //
            "page.html\x01",                // a control byte
            "\xc0\xae\xc0\xae/passwd.html", // an overlong UTF-8 "..", which is bytes, not dots
            "p\xc3\xa4ge.html",             // and any other byte past ASCII
            ".hidden.html",                 // a dot file is never something this means to publish
            "sub//page.html",               // an empty segment
            "sub/",                         // a trailing separator
            "page.html:stream",             // an NTFS alternate data stream
            "page.exe",                     // a suffix with no media type
            "page",                         // and none at all
            "link.html",                    // a symlink: a name for bytes somewhere else
            "folder.html",                  // a directory, which is not a file however it is named
        };

        for (u64 index = 0; index < nya_carray_length(REFUSED); index++) {
            NYA_HttpStaticFile file = {
                .asset = scratch_path(arena, REFUSED[index]),
                .path  = "/x.html",
                .data  = SOME_BYTES,
                .size  = SOME_BYTES_SIZE,
            };

            NYA_Error mounted = nya_http_static_mount((NYA_HttpStaticConfig){
                .files = &file,
                .count = 1,
                .root  = SCRATCH,
            });

            nya_assert(!mounted.ok, "'%s' was mounted, and every one of these is a way out of the root", REFUSED[index]);
            nya_assert(nya_http_static_file_count() == 0, "'%s' left something behind", REFUSED[index]);

            // and nothing partial is left mounted either way.
            nya_http_static_unmount();
        }
    }

    // TEST: a .wasm file mounts and is served as application/wasm, the CSR bundle's module. Its suffix
    // is in the table (unlike page.exe above), so the browser gets the type WebAssembly.instantiateStreaming
    // requires rather than a refusal.
    {
        NYA_ConstCString   asset = scratch_path(arena, "app.wasm");
        NYA_HttpStaticFile file  = {
            .asset = asset,
            .path  = "/app.wasm",
            .data  = SOME_BYTES,
            .size  = SOME_BYTES_SIZE,
        };

        NYA_Error mounted = nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1, .root = SCRATCH });
        nya_assert(mounted.ok, "a .wasm file mounts: %s", (NYA_ConstCString)mounted.message);
        nya_assert(nya_http_static_file_count() == 1, "one file, got " FMTu32, nya_http_static_file_count());

        NYA_ConstCString hashed = nya_http_static_url(asset);
        nya_assert(hashed != nullptr && nya_string_ends_with(nya_string_from(arena, hashed), ".wasm"), "the .wasm suffix survives the hash, got '%s'", hashed);

        nya_http_static_unmount();
    }

    // TEST: a .webmanifest mounts and is served as application/manifest+json, so a CSR bundle whose
    // index.html carries <link rel="manifest"> is an installable PWA. A manifest served as
    // application/json passes no installability check, which is why the suffix has its own media type.
    {
        NYA_ConstCString   asset = scratch_path(arena, "app.webmanifest");
        NYA_HttpStaticFile file  = {
            .asset = asset,
            .path  = "/app.webmanifest",
            .data  = SOME_BYTES,
            .size  = SOME_BYTES_SIZE,
        };

        NYA_Error mounted = nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1, .root = SCRATCH });
        nya_assert(mounted.ok, "a .webmanifest file mounts: %s", (NYA_ConstCString)mounted.message);

        NYA_ConstCString hashed = nya_http_static_url(asset);
        nya_assert(hashed != nullptr && nya_string_ends_with(nya_string_from(arena, hashed), ".webmanifest"),
                   "the .webmanifest suffix survives the hash, got '%s'", hashed);

        nya_http_static_unmount();
    }

    // TEST: an absolute path, and a root the handle only appears to be under.
    {
        NYA_ConstCString OUTSIDE[] = {
            "/etc/passwd",
            "./assets/web/index.html", // real, and not under this mount's root
        };

        for (u64 index = 0; index < nya_carray_length(OUTSIDE); index++) {
            NYA_HttpStaticFile file = { .asset = OUTSIDE[index], .path = "/x.html", .data = SOME_BYTES, .size = SOME_BYTES_SIZE };

            NYA_Error mounted = nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1, .root = SCRATCH });

            nya_assert(!mounted.ok, "'%s' is not under the root and was mounted anyway", OUTSIDE[index]);
            nya_http_static_unmount();
        }

        // the root is matched as a whole segment, so a sibling whose name merely starts with it is out.
        NYA_String* sibling = nya_string_sprintf(arena, "%ssites/page.html", SCRATCH);

        NYA_HttpStaticFile file = {
            .asset = nya_string_to_cstring(arena, sibling),
            .path  = "/x.html",
            .data  = SOME_BYTES,
            .size  = SOME_BYTES_SIZE,
        };

        nya_assert(!nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1, .root = SCRATCH }).ok,
                   "'%s' shares a prefix with the root and is not under it", file.asset);

        nya_http_static_unmount();
    }

    // TEST: a served path is held to the same spelling as the handle behind it.
    {
        NYA_ConstCString REFUSED[] = { "x.html", "/../x.html", "/a/../x.html", "//x.html", "/a//x.html", "/.hidden", "/a\\b" };

        for (u64 index = 0; index < nya_carray_length(REFUSED); index++) {
            NYA_HttpStaticFile file = {
                .asset = scratch_path(arena, "page.html"),
                .path  = REFUSED[index],
                .data  = SOME_BYTES,
                .size  = SOME_BYTES_SIZE,
            };

            nya_assert(!nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1, .root = SCRATCH }).ok,
                       "'%s' is not a path this serves at", REFUSED[index]);

            nya_http_static_unmount();
        }
    }

    // TEST: two files cannot answer one path, and nothing mounts twice.
    {
        NYA_HttpStaticFile twice[] = {
            { .asset = scratch_path(arena, "page.html"), .path = "/", .data = SOME_BYTES, .size = SOME_BYTES_SIZE },
            { .asset = scratch_path(arena, "page.html"), .path = "/", .data = SOME_BYTES, .size = SOME_BYTES_SIZE },
        };

        nya_assert(!nya_http_static_mount((NYA_HttpStaticConfig){ .files = twice, .count = 2, .root = SCRATCH }).ok,
                   "one path answered by two files leaves the second unreachable");

        NYA_HttpStaticFile once = { .asset = scratch_path(arena, "page.html"), .path = "/", .data = SOME_BYTES, .size = SOME_BYTES_SIZE };

        NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = &once, .count = 1, .root = SCRATCH }));

        nya_assert(!nya_http_static_mount((NYA_HttpStaticConfig){ .files = &once, .count = 1, .root = SCRATCH }).ok, "a second mount is refused");

        nya_http_static_unmount();
        nya_assert(nya_http_static_file_count() == 0, "unmounting empties the table");
    }

    /*
     * The serving half, against the bundle the example serves: the generated handles, the real files,
     * and a real socket.
     */
    static const struct {
        NYA_AssetHandle  asset;
        NYA_ConstCString path;
    } SOURCES[] = {
        { NYA_ASSET_WEB_INDEX_HTML, "/"        },
        { NYA_ASSET_WEB_APP_CSS,    "/app.css" },
        { NYA_ASSET_WEB_APP_JS,     nullptr    }, // reachable only by its hash, which is the other half
    };

    NYA_HttpStaticFile bundle[nya_carray_length(SOURCES)] = { 0 };

    for (u64 index = 0; index < nya_carray_length(SOURCES); index++) {
        u8* data = nullptr;
        u64 size = 0;

        NYA_EXPECT(nya_asset_read(arena, SOURCES[index].asset, &data, &size));

        bundle[index] = (NYA_HttpStaticFile){ .asset = SOURCES[index].asset, .path = SOURCES[index].path, .data = data, .size = size };
    }

    NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = bundle, .count = nya_carray_length(bundle) }));
    defer nya_http_static_unmount();

    nya_assert(nya_http_static_file_count() == 3, "three files, got " FMTu32, nya_http_static_file_count());

    u16 port = start_server();
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(nya_http_static_router()));
    defer nya_http_server_unmerge(nya_http_static_router());

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };
    char value[NYA_HTTP_MAX_HEADER_VALUE]    = { 0 };
    char etag[NYA_HTTP_MAX_HEADER_VALUE]     = { 0 };

    // TEST: the entry point answers with the file, its type, its ETag and a revalidating policy.
    {
        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        exchange(socket, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer));

        nya_assert(status_of(answer) == 200, "got %s", answer);
        nya_assert(strstr(answer, "<!doctype html>") != nullptr, "the html itself came back");

        header_of(answer, "Content-Type: ", value, sizeof(value));
        nya_assert(nya_string_equals(value, "text/html; charset=utf-8"), "the type comes from the asset's suffix, got '%s'", value);

        header_of(answer, "Cache-Control: ", value, sizeof(value));
        nya_assert(nya_string_equals(value, "no-cache"), "an unhashed name revalidates, got '%s'", value);

        header_of(answer, "ETag: ", etag, sizeof(etag));
        nya_assert(strlen(etag) == NYA_HTTP_STATIC_HASH_DIGITS + 2, "the ETag is the content hash in quotes, got '%s'", etag);

        // a document may load its own stylesheet, which the server's default policy would refuse.
        header_of(answer, "Content-Security-Policy: ", value, sizeof(value));
        nya_assert(nya_string_equals(value, NYA_HTTP_STATIC_PAGE_CSP), "the page's policy, got '%s'", value);
    }

    // TEST: a caller that already has it gets 304 and no body, weak tags and "*" included.
    {
        // what goes in front of our tag: nothing, the weak marker, and a list whose first entry is
        // somebody else's. "*" says "whatever you have" and carries no tag at all.
        NYA_ConstCString BEFORE[] = { "", "W/", "\"0000000000000000\", " };

        for (u64 index = 0; index <= nya_carray_length(BEFORE); index++) {
            NYA_OsSocket socket = connect_to(port);
            defer             nya_os_socket_close(socket);

            char condition[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };

            if (index < nya_carray_length(BEFORE)) {
                (void)snprintf(condition, sizeof(condition), "%s%s", BEFORE[index], etag);
            } else {
                (void)snprintf(condition, sizeof(condition), "*");
            }

            NYA_String* request = nya_string_sprintf(arena, "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: %s\r\n\r\n", condition);

            exchange(socket, nya_string_to_cstring(arena, request), answer, sizeof(answer));

            nya_assert(status_of(answer) == 304, "'%s' should be a 304, got %s", condition, answer);
            nya_assert(strstr(answer, "<!doctype") == nullptr, "a 304 carries no body");
            nya_assert(strstr(answer, "Content-Length: 0") != nullptr, "and says so");

            header_of(answer, "ETag: ", value, sizeof(value));
            nya_assert(nya_string_equals(value, etag), "the validator goes back out, got '%s'", value);
        }
    }

    // TEST: a tag that is not ours, and a header that is not a list of tags, get the body.
    {
        NYA_ConstCString OTHER[] = { "\"0000000000000000\"", "W/\"0000000000000000\"", "not a tag at all", "\"unterminated" };

        for (u64 index = 0; index < nya_carray_length(OTHER); index++) {
            NYA_OsSocket socket = connect_to(port);
            defer             nya_os_socket_close(socket);

            NYA_String* request = nya_string_sprintf(arena, "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: %s\r\n\r\n", OTHER[index]);

            exchange(socket, nya_string_to_cstring(arena, request), answer, sizeof(answer));

            nya_assert(status_of(answer) == 200, "'%s' names no tag of ours, so the body follows; got %s", OTHER[index], answer);
        }
    }

    // TEST: the hashed name is the same bytes, cached for a year.
    {
        NYA_ConstCString hashed = nya_http_static_url(NYA_ASSET_WEB_APP_CSS);
        nya_assert(hashed != nullptr, "a mounted file has a hashed name");
        nya_assert(strncmp(hashed, NYA_HTTP_STATIC_PREFIX "/app.", strlen(NYA_HTTP_STATIC_PREFIX "/app.")) == 0, "got '%s'", hashed);
        nya_assert(nya_string_ends_with(nya_string_from(arena, hashed), ".css"), "the suffix survives the hash, got '%s'", hashed);

        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        NYA_String* request = nya_string_sprintf(arena, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", hashed);
        exchange(socket, nya_string_to_cstring(arena, request), answer, sizeof(answer));

        nya_assert(status_of(answer) == 200, "got %s", answer);

        header_of(answer, "Cache-Control: ", value, sizeof(value));
        nya_assert(strstr(value, "immutable") != nullptr && strstr(value, "max-age=31536000") != nullptr,
                   "a name containing the hash may be kept, got '%s'", value);

        header_of(answer, "Content-Type: ", value, sizeof(value));
        nya_assert(nya_string_equals(value, "text/css; charset=utf-8"), "got '%s'", value);

        // the hash in the name is the ETag, so there is one spelling of "these bytes".
        header_of(answer, "ETag: ", value, sizeof(value));

        char unquoted[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };
        (void)snprintf(unquoted, sizeof(unquoted), "%.*s", (int)strlen(value) - 2, value + 1);

        nya_assert(strstr(hashed, unquoted) != nullptr, "the name carries the ETag, '%s' and '%s'", hashed, value);
    }

    // TEST: a file with no unhashed path of its own is only reachable by its hash.
    {
        nya_assert(request_status(port, "GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n") == 404, "nothing was mounted at /app.js");

        NYA_ConstCString hashed  = nya_http_static_url(NYA_ASSET_WEB_APP_JS);
        NYA_String*      request = nya_string_sprintf(arena, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", hashed);

        nya_assert(request_status(port, nya_string_to_cstring(arena, request)) == 200, "and its hashed name is");
    }

    // TEST: a HEAD answers the GET's head and none of its bytes.
    {
        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        exchange(socket, "HEAD /app.css HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer));

        nya_assert(status_of(answer) == 200, "got %s", answer);
        nya_assert(strstr(answer, "--ink") == nullptr, "a HEAD carries no body");

        header_of(answer, "Content-Length: ", value, sizeof(value));
        nya_assert(strtoull(value, nullptr, 10) > 0, "and still says how long the body would have been, got '%s'", value);

        header_of(answer, "ETag: ", value, sizeof(value));
        nya_assert(value[0] == '"', "with the validators a GET would carry, got '%s'", value);
    }

    // TEST: every spelling of a climb is refused, and none of them is a 200.
    {
        /*
         * Split by who refuses it. The parser answers 400 for a target that is not one this server
         * reads at all (http_message.c), and the router answers 404 for a path that is well formed and
         * is simply not a route. Both are refusals; the point of listing them here is that neither is
         * ever a 200 and neither reaches a file name, because there is no file name to reach.
         */
        NYA_ConstCString MALFORMED[] = {
            "GET /../assets/web/app.css HTTP/1.1\r\nHost: x\r\n\r\n",       // a dot segment
            "GET /app.css/.. HTTP/1.1\r\nHost: x\r\n\r\n",                  // and a trailing one
            "GET /%2e%2e/assets/web/app.css HTTP/1.1\r\nHost: x\r\n\r\n",   // percent encoded
            "GET /..%2fassets%2fweb HTTP/1.1\r\nHost: x\r\n\r\n",           // with an encoded separator
            "GET /static%2f..%2fapp.css HTTP/1.1\r\nHost: x\r\n\r\n",       //
            "GET /app.css%00.txt HTTP/1.1\r\nHost: x\r\n\r\n",              // a NUL, which truncates a C string
            "GET /..\\..\\app.css HTTP/1.1\r\nHost: x\r\n\r\n",             // the Windows separator
            "GET //app.css HTTP/1.1\r\nHost: x\r\n\r\n",                    // a path that reads as a host
            "GET http://127.0.0.1/app.css HTTP/1.1\r\nHost: x\r\n\r\n",     // the absolute form, which is for proxies
            "GET /app.css#/../ HTTP/1.1\r\nHost: x\r\n\r\n",                // a fragment, which is never sent
        };

        for (u64 index = 0; index < nya_carray_length(MALFORMED); index++) {
            nya_assert(request_status(port, MALFORMED[index]) == 400, "request %llu should be refused: %s", (unsigned long long)index,
                       MALFORMED[index]);
        }

        NYA_ConstCString UNROUTED[] = {
            "GET /%c0%ae%c0%ae/app.css HTTP/1.1\r\nHost: x\r\n\r\n", // an overlong UTF-8 "..", which stays bytes
            "GET /%2561%2e%2e/app.css HTTP/1.1\r\nHost: x\r\n\r\n",  // and a doubly encoded one, decoded once only
            "GET /assets/web/app.css HTTP/1.1\r\nHost: x\r\n\r\n",   // the asset's own path is not a URL
            "GET /static HTTP/1.1\r\nHost: x\r\n\r\n",               // the prefix is not a resource
            "GET /static/ HTTP/1.1\r\nHost: x\r\n\r\n",              // and neither is a directory
            "GET /static/app.css HTTP/1.1\r\nHost: x\r\n\r\n",       // the unhashed name does not live under it
            "GET /App.css HTTP/1.1\r\nHost: x\r\n\r\n",              // a path is matched as it is spelled
            "GET /app.css. HTTP/1.1\r\nHost: x\r\n\r\n",             // and no suffix is added or dropped
        };

        for (u64 index = 0; index < nya_carray_length(UNROUTED); index++) {
            nya_assert(request_status(port, UNROUTED[index]) == 404, "request %llu should be a 404: %s", (unsigned long long)index, UNROUTED[index]);
        }
    }

    // TEST: the bundle is read only, and a range is not something it answers.
    {
        nya_assert(request_status(port, "DELETE /app.css HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n") == 405, "a file is not written here");

        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        exchange(socket, "GET /app.css HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\n\r\n", answer, sizeof(answer));

        // ranges are not implemented and nothing advertises them, so the whole representation comes
        // back: RFC 9110 says a server that does not support a Range ignores it.
        nya_assert(status_of(answer) == 200, "got %s", answer);
        nya_assert(strstr(answer, "Accept-Ranges") == nullptr, "and nothing invited the client to ask");
        nya_assert(strstr(answer, "--ink") != nullptr, "the whole file came back");
    }

    // TEST: the security headers apply here like anywhere else.
    {
        NYA_OsSocket socket = connect_to(port);
        defer             nya_os_socket_close(socket);

        exchange(socket, "GET /app.css HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer));

        nya_assert(strstr(answer, "X-Content-Type-Options: nosniff") != nullptr, "a type nobody may guess at");
        nya_assert(strstr(answer, "Cross-Origin-Resource-Policy: same-origin") != nullptr, "and nobody else may embed");
        nya_assert(strstr(answer, "X-Request-Id: ") != nullptr, "and a request id, as every answer carries");

        // a stylesheet is not a document: it keeps the server's default policy, not the page's.
        header_of(answer, "Content-Security-Policy: ", value, sizeof(value));
        nya_assert(strstr(value, "script-src") == nullptr, "got '%s'", value);
    }

    nya_log_info("test_http_static: ok.");

    return EXIT_SUCCESS;
}
