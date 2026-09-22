/**
 * @file http.h
 *
 * An HTTP/1.1 server inside the program, and the OpenAPI document that describes it, generated from
 * the same tables the server dispatches through.
 *
 * Off unless a program turns it on, which is one call. Until then there is no thread, no socket and
 * no allocation, and a build that never calls nya_system_http_init links the same as before.
 *
 * ```
 * http_types.h     the vocabulary: methods, statuses, media types, requests, responses, and the bounds
 * http_message.h   the wire boundary: bytes in, NYA_HttpRequest out; NYA_HttpResponse in, bytes out
 * http_router.h    routes, the layer chain, the identity extractor, and dispatch
 * http_auth.h      JWT over HMAC-SHA256, the bearer extractor, and the second factor seam
 * http_totp.h      the TOTP second factor: enrolment, recovery codes, and one verification
 * http_server.h    the listener, the connections and the drain
 * http_static.h    the web bundle out of the asset system: hashed names, ETags, one route per file
 * http_websocket.h the RFC 6455 wire format, shared with the curl client in plugins/curl
 * http_websocket_server.h  the upgrade, and a connection that outlives the exchange that made it
 * http_openapi.h   the OpenAPI document and the browsable page, both generated from the route table
 * ../debug/debug_metrics.h   the first resource, this program's own numbers; in debug, since it reads the app loop
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = 7777 }));
 * defer nya_system_http_deinit();
 *
 * NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()));
 * NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()));
 * ```
 *
 * ── the verbs ──
 *
 * QUERY, POST, PUT and DELETE: read, create, update, remove. A read is a QUERY because a request here
 * is a reflected DTO whose schema is generated from the same table the serializer walks, and a GET has
 * nowhere to put one; QUERY is safe and idempotent exactly as GET is, and carries a body, which is the
 * whole of what draft-ietf-httpbis-safe-method-w-body adds.
 *
 * GET still works and is not deprecated: it parses, it routes, a HEAD falls back to it first, and the
 * two routes of http_openapi.h — `GET /openapi.json` and `GET /docs` — stay GET because a browser and
 * a schema generator have no other verb and neither request carries parameters. What changed is which
 * verb a new resource is written in.
 *
 * The one place this leaves the beaten track is the generated schema: OpenAPI gained a `query` field
 * for the Path Item Object in 3.2.0 and had nowhere to put one before that, so the document says
 * 3.2.0. See http_openapi.h, which states what that costs.
 *
 * ── what this is and is not ──
 *
 * It is the server half of "one stack for everything": a nyangine program that serves a web interface
 * for its own metrics, with the DTOs the schema is generated from being the same types a generated
 * client would be generated from, and a WebSocket where a poll is the wrong shape for what it has to
 * say. Connections, requests and sockets are limited per address in process (see http_server.h). TLS
 * and request size policy above these bounds still belong to a proxy in front, until TLS lands in
 * process as TODO.md plans.
 * */
#pragma once

#include "nyangine/http/http_auth.h"
#include "nyangine/http/http_message.h"
#include "nyangine/http/http_openapi.h"
#include "nyangine/http/http_router.h"
#include "nyangine/http/http_server.h"
#include "nyangine/http/http_static.h"
#include "nyangine/http/http_totp.h"
#include "nyangine/http/http_types.h"
#include "nyangine/http/http_websocket.h"
#include "nyangine/http/http_websocket_server.h"
