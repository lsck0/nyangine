# The HTTP server

A nyangine program can serve HTTP: routing, a middleware chain, typed request and response structs,
JSON bodies through `serde`, JWT authentication, WebSockets, an OpenAPI document generated from the
handlers themselves, and a web bundle served out of the asset system with content hashed names and ETags.

It is off unless a program turns it on. Before `nya_system_http_init` there is no thread, no socket
and no allocation, and a build that never calls it links the same as one written before the module
existed. A running server costs the fixed buffers listed under [Bounds](#bounds) and nothing else.

The declarations are in [the cheatsheet](CHEATSHEET.md#http); the headers under `src/nyangine/http/`
are the manual. This page is the shape of the thing.

## Starting one

```c
u8  secret[NYA_HTTP_MAX_SECRET_BYTES] = { 0 };
u64 secret_size                       = 0;

// optional: without it the authenticated routes answer 503 and the open ones still work.
(void)nya_http_secret_from_environment("NYANGINE_HTTP_SECRET", secret, sizeof(secret), &secret_size);

NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
    .port        = 7777,
    .secret      = secret,
    .secret_size = secret_size,
    .layers      = (const NYA_HttpLayerFn[]){ nya_http_layer_log },
    .layer_count = 1,
}));
defer nya_system_http_deinit();

NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()));
NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()));
```

The tick is hooked onto `NYA_EVENT_HANDLING_STARTED`, so a program running the engine's frame loop
needs no second call: a request arrives where a keypress arrives. A program with no frame loop — a
headless tool, a test — calls `nya_system_http_tick` itself, and `nya_system_http_init` notices there
is no app and says so at debug level rather than requiring one.

`gnyame` starts a server when `GNYAME_WEB_PORT` names a port, which is the caller to read:
`src/gnyame/web.c`.

## One drain a frame, or a thread

`workers` in the config decides, and zero is the default.

**`workers = 0`** is what this has always been. `nya_system_http_tick` accepts, reads, answers and
closes on whoever calls it, at most `NYA_HTTP_MAX_REQUESTS_PER_TICK` requests per call, and returns.
No thread is started, nothing is concurrent, nothing takes a lock, and the whole exchange can be
driven a step at a time — which is what `tests/nyangine/http/test_server.c` does and what a simulation
needs, since threads would take its determinism away.

**`workers = n`** starts a listener thread and `n` workers. The listener owns the sockets: it accepts,
reads, parses, spends the address's rate limit token, and writes what comes back. A worker takes a
parsed request and runs the layers, the extractor and the handler on its own arena. A request is then
answered when it arrives rather than when the frame next comes round, and an Argon2id hash or a slow
query costs the frame nothing.

```c
NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = 7777, .workers = 4 }));
```

### A route says where it runs

```c
{
    .method   = NYA_HTTP_METHOD_QUERY,
    .path     = "/api/guild",
    .affinity = NYA_HTTP_AFFINITY_MAIN,   // reads a table the frame writes
    .handler  = guild_read,
}
```

`NYA_HTTP_AFFINITY_WORKER` is the default and is a promise: the handler touches the exchange, its own
arena, and state that is either immutable for the server's lifetime or safe on its own. Not the
program — no entities, no system registry, no renderer, no UI.

**The promise is enforced rather than documented.** The modules that belong to the frame call
`nya_thread_main_only` (`base_thread.h`), which asserts against the thread the app claimed at startup.
A handler that forgot its `MAIN` crashes the first time it runs, naming what it reached for, instead of
corrupting a table weeks later. The system registry is the first module to say so; `core_system.h`
records which of its calls do.

`NYA_HTTP_AFFINITY_MAIN` exchanges are queued and answered inside `nya_system_http_tick`, then handed
back to the listener to write. So a threaded server still has to be ticked, and a `MAIN` handler can
still hold the frame up for as long as it runs — that is the trade it is making on purpose.

The engine's own metrics resource declares `MAIN` for all five of its routes, because every one of them
reads what the frame writes. `examples/web_server/main.c` is the mixed caller to read: its notes and
its second factor are `MAIN`, and the session routes, the web bundle and the generated document run on
workers.

### WebSockets belong to the tick

In both modes. The handshake is answered on the ticking thread and every frame after it is read,
dispatched and written there, so `on_open`, `on_message` and `on_close` run where a program's own state
lives and `nya_http_websocket_broadcast_text` stays a call the frame makes. The listener thread hands
the socket over at the 101 and never touches it again.

### Shutdown

`nya_system_http_deinit` returns with nothing of the server running. The listener is stopped and joined
first, so no socket is read, written or accepted after that point and none is left half closed; then
the workers get `NYA_HTTP_SHUTDOWN_GRACE_MS` between them to leave the handler they are inside.

A handler still running at the deadline cannot be stopped from outside, so the deadline is kept the
only sound way: the port goes back, the thread is detached, and that server's arena and exchange
buffers are leaked on purpose rather than freed under a thread that is still writing into them. It is
logged as an error, because it is a bug in the handler.

## The verbs: QUERY, POST, PUT, DELETE

Read, create, update, remove. A read is a **QUERY**, not a GET.

QUERY is the IETF draft method from `draft-ietf-httpbis-safe-method-w-body`: safe and idempotent
exactly as GET is — it changes nothing, and repeating it is the same request — and it carries a body.
That last part is the whole reason it is here. A request in this codebase is a `@reflect`-annotated
DTO whose JSON Schema is generated from the same table the serializer walks, and there is nowhere in a
GET to put one: a query string is a flat list of strings, and describing one would mean a second,
hand-maintained description of the same request beside the generated one.

GET has not gone anywhere. It is parsed, routed and served, a HEAD falls back to it first, and
`GET /openapi.json` and `GET /docs` stay GET because a browser and a schema generator have no other
verb and neither request has parameters to carry. What changed is which verb a new resource is
written in.

```sh
curl -X QUERY http://127.0.0.1:7777/api/metrics
curl -X QUERY -H 'content-type: application/json' -d '{}' http://127.0.0.1:7777/api/metrics
```

Two rules are enforced rather than remembered:

- A body on a GET, a HEAD or an OPTIONS is refused with 400. RFC 9110 gives those bytes no meaning,
  and an intermediary that counts them as a body while this parser counts them as the start of the
  next request is request smuggling. `Content-Length: 0` is not a body and is fine on any verb.
- `nya_http_router_check` refuses a route with a `request_type` on a verb that carries no body, and a
  route that declares 201 on a verb that changes nothing. Those are the two halves of turning a QUERY
  back into a GET by accident, and neither table starts.

A HEAD with no HEAD route of its own answers from the path's GET, and failing that from its QUERY,
with no body and so no request document. Which is why `HEAD /api/metrics` still works.

## A router per resource

One file per resource. It exports its path constants and one router over a `static const` route
table, and the server merges it at the root. Nothing registers anything at startup; the table is data
the compiler lays out.

```c
#define NYA_HTTP_METRICS_PATH "/api/metrics"

NYA_INTERNAL const NYA_HttpRoute _ROUTES[] = {
    {
        .method        = NYA_HTTP_METHOD_QUERY,
        .path          = NYA_HTTP_METRICS_PATH,
        .auth          = NYA_HTTP_AUTH_NONE,
        .handler       = metrics_query,
        .summary       = "Frame time and this server's own counters",
        .response_type = nya_reflect_of(NYA_HttpMetricsDto),
        .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
    },
};

NYA_INTERNAL const NYA_HttpRouter _ROUTER = {
    .name        = "metrics",
    .routes      = _ROUTES,
    .route_count = nya_carray_length(_ROUTES),
};

const NYA_HttpRouter* nya_http_metrics_router(void) { return &_ROUTER; }
```

Paths match exactly. There are no patterns and no path parameters: one verb per path per shape, with
which instance is being asked for coming out of the request document the QUERY carries. A path pattern
is a second way to say the same thing and a second place for a traversal bug to live. Where two
resources answer two different *shapes*, they are two paths.

`nya_http_server_merge` runs `nya_http_router_check` first, so a table that is not well formed does
not start the resource.

## Every handler documents itself

`auth`, `scope`, `summary`, `description`, `request_type`, `response_type` and `statuses` are not
documentation beside the route. They are the route. The OpenAPI document is generated from them, and
a debug build asserts that an exchange never ends in a status its route did not declare — so a status
list that drifts from the code is a test failure rather than something a reader of the schema finds
out later.

`statuses` covers the whole chain: a route wrapped in a layer that can refuse declares that refusal,
and a route behind the extractor is required to declare 401 and 403 because the extractor can produce
them. The server statuses, 500 and up, are exempt; they say this program is broken, which is true of
every route.

## Layers are cross-cutting, extractors are preconditions

A layer is an onion ring. It gets the exchange and the rest of the chain, does what it likes before
calling `nya_http_chain_next`, and sees the status on the way back out:

```c
NYA_HttpStatus my_layer(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    u64 started = nya_clock_get_monotonic_ns();

    NYA_HttpStatus status = nya_http_chain_next(exchange, next);

    nya_log_info("%llu ns", nya_clock_get_monotonic_ns() - started);

    return status;
}
```

A layer that returns without calling next answers on its own, which is what an authorization layer
does. Root layers wrap the resource's own, which wrap the extractor, which wraps the handler. Layers
take no user data: one is installed by the program that also owns whatever it would read, and a void
pointer per layer is a lifetime question for no gain.

The identity extractor is *not* a layer. It is a precondition of the route, and it is structural:

```c
NYA_HttpStatus settings_put(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity);
```

A handler with that signature can only be registered in `handler_identified`, which
`nya_http_router_check` requires `auth` to be set for, and a route with `auth` set refuses a plain
handler. So a handler taking a caller cannot run unauthenticated, and the rule is the type of the
function pointer rather than something anybody has to remember. The extractor runs after every layer
and immediately before the handler, so a logging layer still sees a 401 and a handler still cannot.

## DTOs are the only thing crossing the wire

A DTO is a `@reflect`-annotated struct. `nya_http_request_reflect` reads a body into one and
`nya_http_response_reflect` writes one out, both through `nya_reflect_to_object` and `serde`. A
handler holds its request type or it holds nothing; it is never handed bytes.

This is the piece everything else hangs off. The schema is generated from the same reflection table
the serializer walks, which is what makes a generated client possible later without the types being
restated anywhere.

## The schema, generated

`GET /openapi.json` is built on every request by walking what is mounted right now. Nothing is
stored, nothing is hand written, and unmounting a resource takes it out of the document.

| In the document      | Comes from                                          |
| :------------------- | :-------------------------------------------------- |
| paths, methods       | `NYA_HttpRoute.path` and `.method`                   |
| summary, description | `NYA_HttpRoute.summary` and `.description`           |
| tags                 | `NYA_HttpRouter.name`, so one resource is one group  |
| security             | `NYA_HttpRoute.auth` and `.scope`                    |
| responses            | `NYA_HttpRoute.statuses`, with the reason phrase     |
| requestBody, schemas | the `NYA_TypeReflection` of each named DTO           |

The schema mirrors the *serializer*, not the C type: `nya_reflect_to_object` writes an enum as its
variant name, a set of bitflags as a list of names, and a `char[N]` as text, so the schema says the
same. A schema describing the C layout would be wrong in exactly the places a generated client would
trust it.

`GET /docs` is the same walk rendered as a page. It is generated in C from the route table, the way
`CHEATSHEET.md` is generated from the headers: no CDN, works offline, and nothing to maintain. It is
deliberately plain, and it is the placeholder that does not need throwing away when the UI backend
can emit HTML from `nya_ui_*` calls.

Both of those routes are GET and stay GET. A person types `/docs` into a browser, every schema
generator fetches `/openapi.json` with GET, and neither request carries parameters, which is the only
thing QUERY buys.

### Where QUERY goes in the document, and what it costs

The document says `"openapi": "3.2.0"`, and that is the one line in the generator that is a judgement
call rather than a walk of the route table.

OpenAPI 3.1's Path Item Object has a fixed set of method fields and `query` is not one of them, so a
3.1 document had three ways to carry a QUERY route and all three are bad:

| Option                         | What happens                                                          |
| :----------------------------- | :-------------------------------------------------------------------- |
| an `x-query` extension          | every validator accepts it and every generator ignores it, so the verb this server reads through is missing from every generated client |
| a bare `query` key under 3.1.0  | a validator rejects the document, and the version string would be a lie |
| leaving the route out           | a generated document that does not describe what is mounted            |

OpenAPI 3.2.0 added `query` to that fixed set, for this method, so the operation sits in the field the
specification gives it and the document claims the version that has it. Nothing else the generator
emits differs between 3.1 and 3.2, so the version is the whole of the change.

What that costs: a tool that only understands 3.1 refuses the document over its version string, rather
than reading it and quietly dropping the routes. A loud no from an old tool beats a silent hole in a
generated client, which is the trade being made. A generated client built from this document gets a
`query` operation and sends `QUERY` with the request DTO as the body; a 3.1-only generator gets an
error it can act on. The version is a `#define` in `http_openapi.h` with this reasoning beside it, and
QUERY is still an IETF draft, so that `#define` and `NYA_HttpMethod` are the two places a change to
the draft would land.

## The web bundle

`http_static.h` serves a page and what it loads. The files are assets, so they get the asset system's
handles, its files in development and its baked blob in a release; the caller reads them with
`nya_asset_read` and hands the bytes over, because `core` sits above `http` in the module order this
tree is moving to and a server that reached into the asset system would invert it.

```c
NYA_HttpStaticFile files[] = {
    { .asset = NYA_ASSET_WEB_INDEX_HTML, .path = "/",        .data = html, .size = html_size },
    { .asset = NYA_ASSET_WEB_APP_CSS,    .path = "/app.css", .data = css,  .size = css_size  },
};

NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = files, .count = nya_carray_length(files) }));
defer nya_http_static_unmount();

NYA_EXPECT(nya_http_server_merge(nya_http_static_router()));
```

`examples/web_server/` is the caller: `/` is a page that reads and writes the notes resource beside it.

### One route per file, and no path resolution anywhere

A mount turns each file into exact routes and nothing else, and the router matches a path by comparing
it whole. A request either **is** one of those strings or it is a 404. There is no root to escape from,
no segment joined onto a directory and no byte of a request that ever becomes part of a file name,
which is the one decision here that matters: path resolution is what static serving gets wrong.

So there are no listings, no implicit `index.html` for a directory, and no "try the path, then the path
plus `.html`". The entry point is the file whose `path` the program wrote down.

The way in that is left is the program itself, building an asset handle out of something a stranger
said. A mount checks every handle before it serves a byte of it: under the root, made only of
`A-Za-z0-9._-` and `/`, no `.` or `..` segment, no empty segment, no leading dot on a segment, and a
suffix this server has a media type for. One rule set refuses `..`, an absolute path, a backslash, a
percent escape, a NUL and every byte an overlong UTF-8 sequence is made of. Where the assets are still
files, a handle naming a symlink or a directory is refused too: a link is a name for bytes somewhere
else, which is the whole thing this is trying not to serve. `tests/nyangine/http/test_static.c` checks
each of those as a refusal, and checks the same spellings over a socket, where the parser answers 400
and the router 404.

### What a name means

The hash is SHA-256 over the bytes as they are served, taken at mount, and its first
`NYA_HTTP_STATIC_HASH_DIGITS` hex digits spell both the name and the ETag. One hash, one spelling.

| Path                     | `Cache-Control`                       | Why                                              |
| :----------------------- | :------------------------------------ | :----------------------------------------------- |
| `/static/app.<hash>.css` | `public, max-age=31536000, immutable` | new bytes are a new name, so it cannot go stale   |
| `/`, `/app.css`          | `no-cache`                            | one name, changing bytes: stored, revalidated     |

Both carry the `ETag`, and both answer a matching `If-None-Match` with `304 Not Modified` and no body,
comparing weakly as RFC 9110 requires, so `W/"x"`, `"x"` and `*` all match. `no-cache` is "store it and
ask me first", not "do not store"; `no-store` would make a browser refetch the entry point on every
navigation for nothing.

There is no `Last-Modified` and no `If-Modified-Since`. The ETag answers the question exactly, a second
validator is a second answer that can disagree with the first, and in a release the bytes come out of
the executable's `.rodata` and have no modification time that is not a fiction.

Content types come from the served file's own suffix, out of a closed table, and a suffix with no media
type in it is refused at mount rather than served as `application/octet-stream`. Nothing a request says
has any say in what a file is.

### No content coding, and no ranges

Responses go out as they are stored: no `Content-Encoding`, and therefore no `Vary: Accept-Encoding`.
The compressor this engine vendors is LZ4, which is not a registered HTTP content coding and which no
browser can decode; gzip and brotli are a dependency decision of their own rather than a side effect of
serving a page. A `Vary` on a response that does not vary only splits every cache entry in two.

The build does compress the bundle where compression pays here: `src/build/pp/asset.c` stores an asset
LZ4-compressed inside the executable when that is smaller, so a release carries the bundle compressed
and the asset system expands it on the read that feeds a mount.

Ranges are not implemented and nothing advertises `Accept-Ranges`. A file is at most
`NYA_HTTP_MAX_STATIC_FILE_BYTES` and leaves in one write, so a `Range` header is ignored and the whole
representation is answered, which is what RFC 9110 says a server without ranges does.

### The policy a page carries

The default `Content-Security-Policy` is `default-src 'none'`, which is right for a JSON body and would
stop a page loading its own stylesheet. An HTML file from the bundle replaces it with
`NYA_HTTP_STATIC_PAGE_CSP`: same origin for scripts, styles, images, fonts and fetches, no inline
anything, and `frame-ancestors`, `base-uri` and `form-action` shut as the default has them. Every other
media type keeps the default, so an SVG opened on its own is a document that may load nothing.

Everything else applies unchanged. The security headers, the origin-agnostic rate limits and the
per-address connection cap are the server's, not a route's, and these routes are routes like any other.

## Authentication

`Authorization: Bearer <jwt>`, HS256 over the crypto module's HMAC-SHA256 (`crypto_hash.h`).

**What is real.** Encoding and decoding, with the checks in this order, which is the security
property: the length bound, then the three-part shape, then the signature in constant time, then the
header's `alg`, and only then the payload. A token that fails any of those has not had its claims
parsed at all, so a forged one never reaches a JSON parser. `alg` is compared against `HS256` whole
rather than read and obeyed, which covers `alg: none` and the downgrade family. base64url is rejected
when its final group's spare bits are not zero, so one signature has exactly one spelling and a token
cannot be edited into a different string that still verifies. A subject is restricted to
`A-Z a-z 0-9 . _ - @`, so nothing has to escape JSON when building the payload.

**What is a seam.** The second factor is half built. The challenge is real and stateless — an HMAC of
the subject and a coarse timestamp under the server secret, so nothing is stored per user and a
restarted server still recognises what it issued. The signature check is not: OpenPGP parsing is not
in this engine and is not going into it as a side effect of an HTTP server.
`nya_http_second_factor_set` installs a verifier, and a route demanding a second factor with none
installed answers 501 rather than passing. Nothing silently succeeds.

**Where the token comes in.** `Authorization: Bearer` first, and then the `__Host-session` cookie when
there is no such header. A cookie is sent by the browser whether the page meant to send it or not,
which is what CSRF is, so the cookie form is only safe behind the two defences this server already
has: dispatch refuses a cross site request that changes anything before any handler runs, and the
cookie is written `SameSite=Strict`, so a browser does not attach it to a cross site request at all.
A header always wins over a cookie, because a caller that sent a header meant to.

**Cookies** (`http_cookie.h`) refuse rather than repair, in both directions. The parser takes exactly
one spelling — `name=value`, separated by `"; "` — and refuses the whole header for anything else: a
quoted value, a comma, a backslash, a control byte, a name twice, more than `NYA_HTTP_MAX_COOKIES`
pairs. A partly read header is the disagreement between two parsers that a stolen session lives in, so
there is no partial answer, and a refused header reads as no cookies at all. Writing refuses a value
that would need quoting rather than quoting it. `__Host-` and `__Secure-` are rules a browser
enforces, so they are enforced here too, where the mistake is still visible: a `__Host-` cookie
without `Secure`, with a `Domain`, or with a `Path` other than `/` is refused at the call rather than
silently dropped by the browser later. The parser is fuzzed, with re-rendering the parsed pairs back
to the original bytes as its oracle.

**The secret** comes from the environment and nowhere else. There is no default, and a secret shorter
than `NYA_HTTP_MIN_SECRET_BYTES` is refused at startup rather than accepted and quietly useless. A
server with no secret serves its open routes and answers 503 on the rest.

## Every request is untrusted

The bounds are the design, not a check bolted on. Every part of a request has a fixed capacity with
the reason for its size written next to it in `http_types.h`, nothing grows, nothing is allocated per
request, and a request that does not fit is answered with the status that says so.

- A head has to end inside `NYA_HTTP_MAX_HEAD_BYTES`, or 431 and close.
- A body is bounded twice: its decoded size, and the number of chunks it may be built from, so a peer
  cannot send eight kilobytes as eight thousand one-byte chunks.
- A request carrying both `Content-Length` and `Transfer-Encoding` is refused. There is no safe
  preference: two intermediaries pick different ones and disagree about where the next request
  starts, which is request smuggling.
- The request target is parsed by `nya_url_parse_target` (`base_url.h`), the same parser everything
  else in the engine uses. A path is percent-decoded *before* the `.` and `..` check, so `%2e%2e` is
  caught by the same rule as `..`. It is refused rather than collapsed: a path that meant to climb is not
  a path this server has a resource for. So are `%2F` inside a segment, a path starting `//`, a
  fragment, and any byte outside RFC 3986.
- A query parameter named twice is no value at all rather than the first or the last, since a proxy
  that reads one and a handler that reads the other is parameter pollution.
- A stream the parser has given up on is closed, never resynchronised. Guessing where the next request
  begins is the same bug again.
- A response header carrying a CR or LF is refused rather than stripped, which is response splitting.
- A peer that goes quiet mid-request is dropped after `NYA_HTTP_IDLE_TIMEOUT_MS`. A peer that stops
  reading is dropped once more than `NYA_HTTP_MAX_PENDING_WRITE_BYTES` is queued for it. A peer past
  the connection table is closed immediately rather than queued.
- At most `NYA_HTTP_MAX_REQUESTS_PER_TICK` are answered, or started, per drain pass across every
  connection, so a pipelining peer cannot take the frame. A connection has at most one exchange in
  flight and its next request is not parsed until the last one is written, so the connection table is
  what bounds the work however fast a peer sends.
- A thread raises no bound. Every limit here is global to the server rather than per worker, because
  the connection table, the rate limit buckets and the counters live on the listener thread and are
  only ever touched there. What a slow handler can hold up is its own connection, and — for a `MAIN`
  route — the frame; not the other connections, the accept loop, the idle timeouts or shutdown.

Nothing a client can send reaches an assertion. A hostile peer is an operating error, and an assert on
one is a denial of service.

`nya_http_request_parse` is a pure function over a byte range, which is why it is a fuzz target:
`tests/fuzz/fuzz_http_request.c`, replayed from a committed corpus on every `./build run test` and
driven by `./build run fuzz http_request`.

### Bounds

| Constant                               | Default | What it holds                                             |
| :------------------------------------- | ------: | :-------------------------------------------------------- |
| `NYA_HTTP_MAX_CONNECTIONS`             |       8 | connections at once                                       |
| `NYA_HTTP_MAX_HEAD_BYTES`              |    4096 | request line and headers together                         |
| `NYA_HTTP_MAX_BODY_BYTES`              |    8192 | request body, chunked framing undone                      |
| `NYA_HTTP_MAX_RESPONSE_BYTES`          |   65536 | one response body; one buffer per exchange in flight      |
| `NYA_HTTP_MAX_HEADERS`                 |      24 | headers kept from a request                               |
| `NYA_HTTP_MAX_CHUNKS`                  |      64 | chunks one body may be built from                         |
| `NYA_HTTP_IDLE_TIMEOUT_MS`             |    5000 | silence before a connection is dropped                    |
| `NYA_HTTP_MAX_REQUESTS_PER_TICK`       |      16 | requests answered, or started, per drain pass             |
| `NYA_HTTP_MAX_WORKERS`                 |       8 | worker threads, and so handlers running at once           |
| `NYA_HTTP_SHUTDOWN_GRACE_MS`           |    2000 | how long deinit waits for a handler to finish             |
| `NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS` |       4 | connections one address may hold                          |
| `NYA_HTTP_DEFAULT_REQUESTS_PER_SECOND` |      20 | refill rate of one address's request bucket               |
| `NYA_HTTP_DEFAULT_REQUEST_BURST`       |      40 | requests one address may send at once                     |
| `NYA_HTTP_MAX_RATE_BUCKETS`            |      64 | addresses tracked; the one touched longest ago makes room |
| `NYA_HTTP_MAX_STATIC_FILES`            |      32 | files one mount of the web bundle serves                  |
| `NYA_HTTP_MAX_STATIC_FILE_BYTES`       |   65536 | one served file; it leaves in one write                   |
| `NYA_HTTP_MAX_STATIC_BYTES`            | 1048576 | every mounted file together, which is the mount's cost    |
| `NYA_HTTP_STATIC_HASH_DIGITS`          |      16 | hex digits of SHA-256 in a hashed name and its ETag       |

Every one is a `#define` a consumer can override from the command line.

## Request ids and the log line

Every answer carries `X-Request-Id`, 64 random bits in hex, refusals included. While a request is
served the server sets it as the thread's log tag (`nya_log_tag_set`), so every line the request causes
reads `[INFO] [req=…] …`, in the file, the terminal and the ring a crash report prints. `nya_http_layer_log`
adds one summary line: method, the matched route's path, status, duration, body bytes in and out, the
caller's network and subject. The route's path and not the request's, so a query string is never
logged; the network and not the address, `/24` for IPv4 and `/48` for IPv6, so the log says where
traffic comes from without saying who. The exchange keeps the full address for what needs it.

## Limits in process

One address holds at most `max_connections_per_address` connections; the next is closed at accept
rather than queued. Each request, once parsed, spends a token from its address's bucket; an empty
bucket answers `429 Too Many Requests` with `Retry-After` and closes. The three values sit in
`NYA_HttpConfig`, zero meaning the default above. The address is the socket's peer and never
`X-Forwarded-For`, so behind a proxy every client shares one budget; trusting a proxy's header waits
for a configured proxy address.

## WebSockets

A path can be a stream instead of a resource. `nya_http_websocket_route_add` mounts one, an upgrade on
it is answered with a 101, and the socket stops being an HTTP connection and starts being a WebSocket
on the same file descriptor:

```c
NYA_INTERNAL void on_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size) {
    if (is_text && size == 3 && nya_memcmp(data, "now", 3) == 0) push_a_snapshot(socket);
}

NYA_INTERNAL const NYA_HttpWebSocketRoute STREAM = {
    .path       = "/ws/notes",
    .summary    = "a snapshot of the notes and this server, pushed",
    .on_open    = stream_open,
    .on_message = on_message,
};

NYA_EXPECT(nya_http_websocket_route_add(&STREAM));
defer nya_http_websocket_route_remove(&STREAM);

// from the program's own loop, whenever there is something to say
(void)nya_http_websocket_broadcast_text("/ws/notes", json);
```

`examples/web_server/main.c` is the caller to read: it pushes a snapshot when a peer connects, again
whenever a note is written or removed, and once a second regardless.

### One codec, two ends

The framing, the masking, the fragment assembly, the pongs and the close codes are `http_websocket.h`,
and both ends of this engine run it: the server here and the `ws://`/`wss://` client in
`plugins/curl/websocket.h`. There is one frame decoder in the tree, which is the only way two ends can
be relied on to refuse the same things. The codec links no socket — it is bytes in and bytes out over
buffers the caller owns — which is why the plugin can reach up into `http` for it and why it is what
the fuzzer drives.

`nya_websocket_protocol_open` gives a connection its framing over two buffers, `_receive` takes bytes
and hands back whole messages, `_send` frames one, `_close` says goodbye, and `_pending`/`_flushed` are
what a caller moves between it and its socket. A server-side handler reaches the same calls through
`nya_http_websocket_protocol`, so sending a binary message or a ping from a handler is the same call
the client end makes.

### The upgrade is a request like any other

It arrives on the same listener, is parsed by the same parser, and spends a token from its address's
bucket before it is looked at. **A socket does not escape the server's limits by becoming a
WebSocket**: it keeps the connection slot it was accepted into, and it is counted again against the
WebSocket bounds below.

The handshake is answered on the ticking thread even when the server has workers, because what it
registers into is the table above — see [One drain a frame, or a thread](#one-drain-a-frame-or-a-thread).

It also goes through the origin check (`Sec-Fetch-Site`, then `Origin` against `Host`) that every
unsafe request goes through. An upgrade is a GET, so nothing about the method would have brought it
there, and a socket a page on another site opened would read everything the program pushes for as long
as it stayed open. **A cross-site upgrade is refused with 403.**

| Refused                                                            | Answer |
| :----------------------------------------------------------------- | :----- |
| a path with no stream mounted                                       | 404    |
| an upgrade from another site                                        | 403    |
| a handshake that is not one: not a GET, no `Sec-WebSocket-Key`, a key that is not a key, a version that is not 13 | 400 |
| a byte of anything sent after the request and before the 101        | 400    |
| the table, or this address's share of it, already full              | 503    |

The last 400 is worth its own line: RFC 6455 says a client sends nothing until the 101 comes back, so
bytes already in the buffer are either a client that does not follow the protocol or a request being
smuggled through whatever is in front of this server, and neither is worth telling apart. 426 would be
the RFC's answer for the version, and this server's status set has no 426; the version a client must
use is in the problem body instead.

### What a connected peer may do

Anything, inside the bounds. A client frame is always masked and a server frame never is, and a frame
the wrong way round is a close with 1002 rather than something tolerated — an endpoint that accepts
both is one whose own traffic can be replayed back at it. A reserved bit, an unknown opcode, a control
frame that is fragmented or over 125 bytes, a length that is not in its shortest form, text that is not
UTF-8 and a close code the RFC reserves are each a close with the code that says which. None of them
assert, and none of them allocate.

A peer that goes quiet is pinged after `NYA_HTTP_WEBSOCKET_PING_INTERVAL_MS` and dropped after
`NYA_HTTP_WEBSOCKET_IDLE_TIMEOUT_MS`, so a connection whose other end is gone without a FIN — a laptop
that slept, a NAT that dropped the entry — cannot be held forever. A peer that stops reading is dropped
once more than `NYA_HTTP_MAX_PENDING_WRITE_BYTES` is queued for it, which is the bound an HTTP answer
gets.

`nya_websocket_protocol_receive` is a pure function over a byte range and a caller's buffers, which is
why it is the second fuzz target: `tests/fuzz/fuzz_websocket_frame.c`, replayed from a committed corpus
on every `./build run test` and driven by `./build run fuzz websocket_frame`. It feeds the same bytes
to both roles, and then to a server a byte at a time, because a decoder that reads a header differently
across a split is one a peer can steer by choosing its packet sizes.

### Bounds

| Constant                               | Default | What it holds                                            |
| :------------------------------------- | ------: | :------------------------------------------------------- |
| `NYA_HTTP_MAX_WEBSOCKETS`              |       4 | sockets at once, out of the 8 connections                 |
| `NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS`  |       2 | sockets one address may hold                              |
| `NYA_HTTP_MAX_WEBSOCKET_ROUTES`        |       4 | streams a program may mount                               |
| `NYA_HTTP_WEBSOCKET_MAX_FRAME_BYTES`   |    4096 | one frame's payload, refused on the header alone          |
| `NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES` |    8192 | one message, every fragment counted                       |
| `NYA_WEBSOCKET_MAX_FRAGMENTS`          |      64 | fragments one message may be built from                   |
| `NYA_HTTP_WEBSOCKET_SEND_BYTES`        |   16384 | queued outgoing bytes per socket                          |
| `NYA_HTTP_WEBSOCKET_RECEIVE_BYTES`     |    4096 | one read off one socket                                   |
| `NYA_HTTP_WEBSOCKET_IDLE_TIMEOUT_MS`   |   30000 | silence before a socket is dropped                        |
| `NYA_HTTP_WEBSOCKET_PING_INTERVAL_MS`  |   10000 | silence before the server asks whether anyone is there    |
| `NYA_HTTP_WEBSOCKET_MAX_MESSAGES_PER_TICK` |   8 | messages one peer is handed per drain                     |

A socket costs its receive, send and message buffers, about twenty eight kilobytes at the defaults, and
the table is allocated when the first stream is mounted and not before.

### What is not implemented

- **Authentication.** A stream is open to anyone who can reach the port, which is what the loopback
  bind and the origin check are doing the work of. A browser cannot set an `Authorization` header on a
  WebSocket, so a token would have to arrive in the query string or in the first message, and neither
  is a decision to make as a side effect of adding framing.
- **Subprotocol negotiation.** A `Sec-WebSocket-Protocol` offer is ignored rather than answered, which
  RFC 6455 allows and which means a client must not require one.
- **permessage-deflate**, and every other extension. `Sec-WebSocket-Extensions` is never negotiated, so
  the three reserved bits stay zero and a frame that sets one is refused.
- **Sending a message in fragments.** Receiving one is supported, because a peer's fragmentation is not
  ours to decide; nothing here needs to send one.
- **The OpenAPI document** does not describe a stream. The specification describes requests and
  answers, and this is neither, so a mounted route is documented by its `summary` and in prose.

## What belongs to a proxy

TLS, and any request bound above the ones here. Bind to loopback and put a proxy in front. This is the server half of "one stack for everything", not a public-facing web server,
and that is a decision recorded in `TODO.md` rather than an omission.

## The first resource

`nya_http_metrics_router` serves this program's own numbers, and measures nothing new: every value is
already queryable through `nya_app_get`, the ceiling registry, the arena registry and
`nya_system_owner_stats_at`.

```
QUERY /api/metrics              frame time, and the server's own counters
QUERY /api/metrics/ceilings     every fixed capacity array and how full it is
QUERY /api/metrics/arenas       every live arena: used, reserved, fragmentation
QUERY /api/metrics/systems      per owner: how many systems, what they cost, what they hold
PUT   /api/metrics/accounting   turns the registry's per system timing on and off
```

The `PUT` is the only route behind the extractor. It needs `NYA_HTTP_SCOPE_WRITE`, and it is there
because per-system timing costs a clock read per system per phase: a thing to ask for rather than a
thing to report. It is a PUT rather than a POST because it sets a flag to a value — it creates
nothing, sending it twice reaches the same state, and it answers with the state that was reached.

The four reads take no request document yet and a QUERY with no body is the whole request. Asking one
of them for a subset later is a `request_type` on the route and nothing else, which is the reason they
are QUERY today.
