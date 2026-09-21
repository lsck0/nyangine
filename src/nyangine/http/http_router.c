#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/http/http_router.h"
#include "nyangine/platform/clock/clock.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The extractor and then the handler: what the innermost step of the chain is. */
NYA_INTERNAL NYA_HttpStatus _nya_http_router_run_route(NYA_HttpExchange* exchange);

/** Verifies the bearer token on the exchange into `exchange->identity`. The precondition itself. */
NYA_INTERNAL NYA_HttpStatus _nya_http_router_extract_identity(NYA_HttpExchange* exchange);

/** Whether `route` declares `status`. Debug only; see the assertion in nya_http_router_dispatch. */
NYA_INTERNAL b8 _nya_http_route_declares(const NYA_HttpRoute* route, NYA_HttpStatus status) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_http_router_check(const NYA_HttpRouter* router) {
    if (router == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a router is not null");

    if (router->name == nullptr || router->name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a router needs a name for its OpenAPI tag");

    if (router->routes == nullptr || router->route_count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has no routes", router->name);

    if (router->layer_count > NYA_HTTP_MAX_LAYERS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' carries more than %d layers", router->name, NYA_HTTP_MAX_LAYERS);
    }

    if (router->layer_count > 0 && router->layers == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' counts layers it does not have", router->name);
    }

    for (u32 index = 0; index < router->route_count; index++) {
        const NYA_HttpRoute* route = &router->routes[index];

        if (!nya_http_method_is_valid(route->method))
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "route %u of '%s' has no method", index, router->name);

        if (route->path == nullptr || route->path[0] != '/') {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "route %u of '%s' needs an absolute path", index, router->name);
        }

        if (route->summary == nullptr || route->summary[0] == '\0') {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s has no summary, so the schema would have none either",
                nya_http_method_text(route->method),
                route->path
            );
        }

        /*
         * The one rule that makes "a handler taking a caller cannot run unauthenticated" true rather
         * than merely intended: the slot a handler sits in and the route's `auth` have to agree, and a
         * table that gets it wrong does not start.
         */
        if (route->auth == NYA_HTTP_AUTH_NONE) {
            if (route->handler == nullptr || route->handler_identified != nullptr) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "%s %s takes no identity, so it needs `handler` and only `handler`",
                    nya_http_method_text(route->method),
                    route->path
                );
            }
        } else {
            if (route->handler_identified == nullptr || route->handler != nullptr) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "%s %s demands an identity, so it needs `handler_identified` and only that",
                    nya_http_method_text(route->method),
                    route->path
                );
            }
        }

        if (route->statuses[0] == NYA_HTTP_STATUS_NONE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s %s lists no statuses", nya_http_method_text(route->method), route->path);
        }

        /*
         * A route behind the extractor can be refused by it, so it declares those two whether or not
         * its handler ever produces them. Otherwise the generated schema would describe a route that
         * answers 200 and nothing else, which is not what a caller without a token will see.
         */
        if (route->auth != NYA_HTTP_AUTH_NONE &&
            (!_nya_http_route_declares(route, NYA_HTTP_STATUS_UNAUTHORIZED) || !_nya_http_route_declares(route, NYA_HTTP_STATUS_FORBIDDEN))) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s is behind the extractor, so it has to declare 401 and 403",
                nya_http_method_text(route->method),
                route->path
            );
        }

        for (u32 status = 0; status < NYA_HTTP_MAX_STATUSES && route->statuses[status] != NYA_HTTP_STATUS_NONE; status++) {
            if (nya_http_status_is_valid(route->statuses[status])) continue;

            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s lists status %d, which this server cannot answer with",
                nya_http_method_text(route->method),
                route->path,
                (s32)route->statuses[status]
            );
        }
    }

    return NYA_OK;
}

const NYA_HttpRoute*
nya_http_router_find(const NYA_HttpRouter* const* routers, u32 router_count, NYA_HttpMethod method, NYA_ConstCString path, b8* out_path_exists) {
    nya_assert(out_path_exists != nullptr);
    nya_assert(path != nullptr);

    *out_path_exists = false;

    // a HEAD is a GET whose body is dropped on the way out, so it matches the GET route unless the
    // resource wrote a HEAD of its own.
    NYA_HttpMethod fallback = method == NYA_HTTP_METHOD_HEAD ? NYA_HTTP_METHOD_GET : method;

    const NYA_HttpRoute* matched = nullptr;

    for (u32 index = 0; index < router_count && index < NYA_HTTP_MAX_ROUTERS; index++) {
        const NYA_HttpRouter* router = routers[index];
        if (router == nullptr) continue;

        for (u32 position = 0; position < router->route_count; position++) {
            const NYA_HttpRoute* route = &router->routes[position];

            if (!nya_string_equals(route->path, path)) continue;

            *out_path_exists = true;

            if (route->method == method) return route;

            if (route->method == fallback && matched == nullptr) matched = route;
        }
    }

    return matched;
}

NYA_HttpStatus nya_http_router_dispatch(
    NYA_HttpExchange*            exchange,
    const NYA_HttpRouter* const* routers,
    u32                          router_count,
    const NYA_HttpLayerFn*       layers,
    u32                          layer_count
) {
    nya_assert(exchange != nullptr);
    nya_assert(exchange->request != nullptr);
    nya_assert(exchange->response != nullptr);
    nya_assert(layer_count <= NYA_HTTP_MAX_LAYERS);

    b8 path_exists = false;

    exchange->route = nya_http_router_find(routers, router_count, exchange->request->method, exchange->request->path, &path_exists);

    if (exchange->route == nullptr) {
        if (path_exists) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_METHOD_NOT_ALLOWED, "that path does not answer this method");

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_NOT_FOUND, "no route for that path");
    }

    /*
     * Root layers outside the resource's own, both flattened into one array so the chain is a single
     * index rather than two nested walks. Bounded by construction: both halves are checked at merge.
     */
    NYA_HttpLayerFn chain_layers[NYA_HTTP_MAX_LAYERS * 2] = { 0 };
    u32             chain_count                           = 0;

    for (u32 index = 0; index < layer_count && index < NYA_HTTP_MAX_LAYERS; index++) {
        if (layers[index] == nullptr) continue;

        chain_layers[chain_count++] = layers[index];
    }

    for (u32 index = 0; index < router_count && index < NYA_HTTP_MAX_ROUTERS; index++) {
        const NYA_HttpRouter* router = routers[index];
        if (router == nullptr) continue;

        // the router the matched route came from, found by address: the route table is a contiguous
        // array, so a pointer inside it identifies its owner without a back pointer per route.
        if (exchange->route < router->routes || exchange->route >= router->routes + router->route_count) continue;

        for (u32 layer = 0; layer < router->layer_count && layer < NYA_HTTP_MAX_LAYERS; layer++) {
            if (router->layers[layer] == nullptr) continue;

            chain_layers[chain_count++] = router->layers[layer];
        }

        break;
    }

    NYA_HttpChain chain = { .layers = chain_layers, .count = chain_count, .index = 0 };

    NYA_HttpStatus status = nya_http_chain_next(exchange, &chain);

    /*
     * The documented status list, enforced. A handler that answers with something its route does not
     * declare has made the OpenAPI document wrong, and in a build with assertions on that is a test
     * failure rather than something a reader of the schema finds out later.
     */
    nya_assert(
        status >= NYA_HTTP_STATUS_INTERNAL_ERROR || _nya_http_route_declares(exchange->route, status),
        "%s %s answered %d, which its route does not declare",
        nya_http_method_text(exchange->route->method),
        exchange->route->path,
        (s32)status
    );

    // a refusal with nothing in it is a status a client has to guess about. One shape, always.
    if (status >= NYA_HTTP_STATUS_BAD_REQUEST && exchange->response->body_size == 0) {
        return nya_http_response_problem(exchange, status, nya_http_status_text(status));
    }

    return status;
}

NYA_HttpStatus nya_http_chain_next(NYA_HttpExchange* exchange, NYA_HttpChain* chain) {
    nya_assert(exchange != nullptr);
    nya_assert(chain != nullptr);

    if (chain->index >= chain->count) return _nya_http_router_run_route(exchange);

    NYA_HttpLayerFn layer = chain->layers[chain->index];
    chain->index++;

    nya_assert(layer != nullptr, "a null layer reached the chain; nya_http_router_dispatch drops those");

    return layer(exchange, chain);
}

NYA_HttpStatus nya_http_layer_log(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    u64 started_ns = nya_clock_get_monotonic_ns();

    NYA_HttpStatus status = nya_http_chain_next(exchange, next);

    u64 elapsed_us = (nya_clock_get_monotonic_ns() - started_ns) / 1000;

    nya_log_info(
        "%s %s -> %d (%llu us)",
        nya_http_method_text(exchange->request->method),
        exchange->request->path,
        (s32)status,
        (unsigned long long)elapsed_us
    );

    return status;
}

NYA_HttpStatus nya_http_response_problem(NYA_HttpExchange* exchange, NYA_HttpStatus status, NYA_ConstCString detail) {
    nya_assert(exchange != nullptr);
    nya_assert(exchange->response != nullptr);
    nya_assert(nya_http_status_is_valid(status));

    NYA_HttpProblem problem = { .status = (u32)status };

    (void)snprintf(problem.error, sizeof(problem.error), "%s", nya_http_status_text(status));
    (void)snprintf(problem.detail, sizeof(problem.detail), "%s", detail != nullptr ? detail : "");

    nya_http_response_reset(exchange->response);

    NYA_Error written = nya_http_response_reflect(exchange->response, exchange->arena, nya_reflect_of(NYA_HttpProblem), &problem);

    if (!written.ok) {
        // the error body itself would not fit or would not serialize, which is our bug and not the
        // caller's. The status still goes out; there is just nothing to read with it.
        nya_log_error("The %d problem body could not be written.", (s32)status);
        nya_http_response_reset(exchange->response);
    }

    return status;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_HttpStatus _nya_http_router_run_route(NYA_HttpExchange* exchange) {
    const NYA_HttpRoute* route = exchange->route;

    nya_assert(route != nullptr, "the chain reached its innermost step with no route");

    if (route->auth == NYA_HTTP_AUTH_NONE) {
        nya_assert(route->handler != nullptr, "nya_http_router_check refuses a route with no handler");

        return route->handler(exchange);
    }

    NYA_HttpStatus refused = _nya_http_router_extract_identity(exchange);
    if (refused != NYA_HTTP_STATUS_NONE) return refused;

    nya_assert(route->handler_identified != nullptr, "nya_http_router_check refuses a route with no handler");
    nya_assert(exchange->identified, "the extractor returned no refusal and no identity");

    return route->handler_identified(exchange, &exchange->identity);
}

NYA_HttpStatus _nya_http_router_extract_identity(NYA_HttpExchange* exchange) {
    const NYA_HttpRoute* route = exchange->route;

    exchange->identified = false;
    exchange->identity   = (NYA_HttpIdentity){ 0 };

    if (exchange->secret == nullptr || exchange->secret_size < NYA_HTTP_MIN_SECRET_BYTES) {
        // a route that needs a token on a server that cannot verify one. Not the caller's fault and not
        // something they can fix by trying again with credentials.
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "this server was started without a signing secret");
    }

    const char* token      = nullptr;
    u64         token_size = 0;

    if (!nya_http_bearer_token(exchange->request, &token, &token_size)) {
        NYA_Error announced = nya_http_response_header(exchange->response, "WWW-Authenticate", "Bearer");
        if (!announced.ok) nya_log_warn("The WWW-Authenticate header could not be added to a 401.");

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNAUTHORIZED, "this route needs a bearer token");
    }

    NYA_Error verified =
        nya_http_jwt_decode(exchange->arena, token, token_size, exchange->secret, exchange->secret_size, exchange->now_s, &exchange->identity);

    if (!verified.ok) {
        /*
         * One answer for every way a token can be wrong. Telling a caller that their signature was
         * fine but their token had expired is telling an attacker that their forgery verified.
         */
        exchange->identity = (NYA_HttpIdentity){ 0 };

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNAUTHORIZED, "that token is not valid");
    }

    if ((route->scope & NYA_HTTP_SCOPE_SECOND_FACTOR) != 0 && nya_http_second_factor() == nullptr) {
        exchange->identity = (NYA_HttpIdentity){ 0 };

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_NOT_IMPLEMENTED, "this route needs a second factor and none is configured");
    }

    if (!nya_http_scope_contains(&exchange->identity, route->scope)) {
        exchange->identity = (NYA_HttpIdentity){ 0 };

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "that token does not carry the scope this route needs");
    }

    exchange->identified = true;

    return NYA_HTTP_STATUS_NONE;
}

b8 _nya_http_route_declares(const NYA_HttpRoute* route, NYA_HttpStatus status) {
    for (u32 index = 0; index < NYA_HTTP_MAX_STATUSES && route->statuses[index] != NYA_HTTP_STATUS_NONE; index++) {
        if (route->statuses[index] == status) return true;
    }

    return false;
}
