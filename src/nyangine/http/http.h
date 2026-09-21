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
 * http_server.h    the listener, the connections and the drain
 * http_openapi.h   the OpenAPI document and the browsable page, both generated from the route table
 * http_metrics.h   the first resource: this program's own frame time, ceilings, arenas and systems
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
 * ── what this is and is not ──
 *
 * It is the server half of "one stack for everything": a nyangine program that serves a web interface
 * for its own metrics, with the DTOs the schema is generated from being the same types a generated
 * client would be generated from. It is not a public-facing web server. Rate limiting, TLS, request
 * size policy above these bounds and everything else on the perimeter belong to a proxy in front,
 * which is a decision recorded in TODO.md and not an omission.
 * */
#pragma once

#include "nyangine/http/http_auth.h"
#include "nyangine/http/http_message.h"
#include "nyangine/http/http_metrics.h"
#include "nyangine/http/http_openapi.h"
#include "nyangine/http/http_router.h"
#include "nyangine/http/http_server.h"
#include "nyangine/http/http_types.h"
