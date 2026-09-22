# The HTTP server

A nyangine program can serve HTTP: routing, a middleware chain, typed request and response structs,
JSON bodies through `serde`, JWT authentication, and an OpenAPI document generated from the handlers
themselves.

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

The drain is hooked onto `NYA_EVENT_HANDLING_STARTED`, so a program running the engine's frame loop
needs no second call: a request arrives where a keypress arrives. A program with no frame loop — a
headless tool, a test — calls `nya_system_http_tick` itself, and `nya_system_http_init` notices there
is no app and says so at debug level rather than requiring one.

`gnyame` starts a server when `GNYAME_WEB_PORT` names a port, which is the caller to read:
`src/gnyame/web.c`.

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

## Authentication

`Authorization: Bearer <jwt>`, HS256 over `base_hash.c`'s HMAC-SHA256.

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
- At most `NYA_HTTP_MAX_REQUESTS_PER_TICK` are answered per frame across every connection, so a
  pipelining peer cannot take the frame.

Nothing a client can send reaches an assertion. A hostile peer is an operating error, and an assert on
one is a denial of service.

`nya_http_request_parse` is a pure function over a byte range, which is why it is the fuzz target:
`tests/fuzz/fuzz_http_request.c`, replayed from a committed corpus on every `./build run test` and
driven by `./build run fuzz http_request`.

### Bounds

| Constant                            | Default | What it holds                                  |
| :---------------------------------- | ------: | :--------------------------------------------- |
| `NYA_HTTP_MAX_CONNECTIONS`          |       8 | connections at once                            |
| `NYA_HTTP_MAX_HEAD_BYTES`           |    4096 | request line and headers together              |
| `NYA_HTTP_MAX_BODY_BYTES`           |    8192 | request body, chunked framing undone           |
| `NYA_HTTP_MAX_RESPONSE_BYTES`       |   65536 | one response body; one buffer, shared          |
| `NYA_HTTP_MAX_HEADERS`              |      24 | headers kept from a request                    |
| `NYA_HTTP_MAX_CHUNKS`               |      64 | chunks one body may be built from              |
| `NYA_HTTP_IDLE_TIMEOUT_MS`          |    5000 | silence before a connection is dropped         |
| `NYA_HTTP_MAX_REQUESTS_PER_TICK`    |      16 | requests answered per frame, across every peer |

Every one is a `#define` a consumer can override from the command line.

## What belongs to a proxy

TLS, rate limiting per address, and any request bound above the ones here. Bind to loopback and put a
proxy in front. This is the server half of "one stack for everything", not a public-facing web server,
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
