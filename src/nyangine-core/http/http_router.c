#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-std/base/base_clock.h"

// CONSTANTS

/**
 * What a HEAD answers from when the resource wrote no HEAD of its own, most preferred first.
 *
 * A HEAD is a read whose body is dropped on the way out, so it answers from a read route. The GET
 * first, because a GET route is written to be answered with nothing but its path; the QUERY second,
 * because a HEAD carries no body and its handler therefore sees an empty request document. Neither is
 * a write, which the assertion in nya_http_router_find is there to keep true.
 * */
NYA_INTERNAL const NYA_HttpMethod _NYA_HTTP_HEAD_FALLBACK[] = { NYA_HTTP_METHOD_GET, NYA_HTTP_METHOD_QUERY };

// PRIVATE API DECLARATION

/** The table every route's permission is resolved against, and how an identity becomes a subject in it. */
NYA_INTERNAL struct {
    NYA_Permissions* permissions;
    u64 (*subject_of)(const NYA_HttpIdentity* identity);
} _NYA_HTTP_PERMISSIONS = { 0 };

/** The extractor and then the handler: what the innermost step of the chain is. */
NYA_INTERNAL NYA_HttpStatus _nya_http_router_run_route(NYA_HttpExchange* exchange);

/** Verifies the bearer token on the exchange into `exchange->identity`. The precondition itself. */
NYA_INTERNAL NYA_HttpStatus _nya_http_router_extract_identity(NYA_HttpExchange* exchange);

/** Whether `route` declares `status`. Debug only; see the assertion in nya_http_router_dispatch. */
NYA_INTERNAL b8 _nya_http_route_declares(const NYA_HttpRoute* route, NYA_HttpStatus status) __attr_no_discard;

/**
 * Whether a request that changes something came from another site, which is the CSRF case. A browser says so in
 * Sec-Fetch-Site, and an Origin naming a host other than the one the request was sent to says the same. A request
 * with neither is not from a browser page and carries no cookie it did not mean to, so it passes; SameSite=Strict
 * on the session cookies is the second line behind this one.
 * */
NYA_INTERNAL b8 _nya_http_request_is_cross_site(const NYA_HttpRequest* request) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

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

        // The one rule that makes "a handler taking a caller cannot run unauthenticated" true rather than intended: the slot a handler sits in and the route's `auth` must agree, and a table that gets it wrong doesn't start.
        if (route->auth == NYA_HTTP_AUTH_NONE) {
            // Exactly one of the plain pointer or the reload-safe token, and nothing in the identified slots.
            b8 plain = route->handler != nullptr;
            b8 token = route->handler_callback != 0;

            if (plain == token || route->handler_identified != nullptr || route->handler_identified_callback != 0) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "%s %s takes no identity, so it needs exactly one of `handler` or `handler_callback` and no identified handler",
                    nya_http_method_text(route->method),
                    route->path
                );
            }
        } else {
            b8 plain = route->handler_identified != nullptr;
            b8 token = route->handler_identified_callback != 0;

            if (plain == token || route->handler != nullptr || route->handler_callback != 0) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "%s %s demands an identity, so it needs exactly one of `handler_identified` or `handler_identified_callback` and no unauthenticated handler",
                    nya_http_method_text(route->method),
                    route->path
                );
            }
        }

        // A request DTO on a verb with no body is a route nothing can call: the parser refuses the body before the dispatcher; it's what a QUERY route turned back into a GET looks like, caught at merge rather than at the first request.
        if (route->request_type != nullptr && !nya_http_method_allows_body(route->method)) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s takes a %s, and a %s carries no body; QUERY is the read that does",
                nya_http_method_text(route->method),
                route->path,
                route->request_type->name,
                nya_http_method_text(route->method)
            );
        }

        // the other half of that mistake: a safe verb that says it creates things.
        if (nya_http_method_is_safe(route->method) && _nya_http_route_declares(route, NYA_HTTP_STATUS_CREATED)) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s answers 201, which a verb that changes nothing cannot",
                nya_http_method_text(route->method),
                route->path
            );
        }

        if (route->statuses[0] == NYA_HTTP_STATUS_NONE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s %s lists no statuses", nya_http_method_text(route->method), route->path);
        }

        // the cross-site check in dispatch refuses a write from another site, so a route that writes declares it.
        if (!nya_http_method_is_safe(route->method) && !_nya_http_route_declares(route, NYA_HTTP_STATUS_FORBIDDEN)) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s changes something, so the cross-site check can refuse it and it has to declare 403",
                nya_http_method_text(route->method),
                route->path
            );
        }

        // A route behind the extractor can be refused by it, so it declares those two statuses whether or not its handler produces them; else the generated schema would describe a route that answers 200 and nothing else, not what a caller without a token sees.
        if (route->auth != NYA_HTTP_AUTH_NONE &&
            (!_nya_http_route_declares(route, NYA_HTTP_STATUS_UNAUTHORIZED) || !_nya_http_route_declares(route, NYA_HTTP_STATUS_FORBIDDEN))) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "%s %s is behind the extractor, so it has to declare 401 and 403",
                nya_http_method_text(route->method),
                route->path
            );
        }

        // a permission is only answerable for somebody the extractor has identified, and a route asking for one can be told no by a server with no table, so it says so in its statuses too.
        if (route->permission != NYA_PERMISSION_NONE && route->auth == NYA_HTTP_AUTH_NONE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s %s demands a permission, so it needs an identity to resolve one for",
                             nya_http_method_text(route->method), route->path);
        }

        if (route->permission != NYA_PERMISSION_NONE && !_nya_http_route_declares(route, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s %s demands a permission, so it has to declare 503 for a server with no table",
                             nya_http_method_text(route->method), route->path);
        }

        if (route->resource_of != nullptr && route->permission == NYA_PERMISSION_NONE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s %s works out a resource it never checks a permission against",
                             nya_http_method_text(route->method), route->path);
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

    const NYA_HttpRoute* matched = nullptr;

    // how far down _NYA_HTTP_HEAD_FALLBACK the best match so far sits; the length of it means "none".
    u32 rank = nya_carray_length(_NYA_HTTP_HEAD_FALLBACK);

    for (u32 index = 0; index < router_count && index < NYA_HTTP_MAX_ROUTERS; index++) {
        const NYA_HttpRouter* router = routers[index];
        if (router == nullptr) continue;

        for (u32 position = 0; position < router->route_count; position++) {
            const NYA_HttpRoute* route = &router->routes[position];

            if (!nya_string_equals(route->path, path)) continue;

            *out_path_exists = true;

            if (route->method == method) return route;

            if (method != NYA_HTTP_METHOD_HEAD) continue;

            for (u32 candidate = 0; candidate < rank; candidate++) {
                if (_NYA_HTTP_HEAD_FALLBACK[candidate] != route->method) continue;

                nya_assert(nya_http_method_is_safe(route->method), "a HEAD would answer from a route that changes something");

                matched = route;
                rank    = candidate;

                break;
            }
        }
    }

    return matched;
}

void nya_http_permissions_set(NYA_Permissions* permissions, u64 (*subject_of)(const NYA_HttpIdentity* identity)) {
    // both or neither: a table with no way to name a subject can't answer anything, and a resolver with no table has nothing to ask.
    nya_assert((permissions == nullptr) == (subject_of == nullptr), "a permission table and its subject resolver are installed together");

    _NYA_HTTP_PERMISSIONS.permissions = permissions;
    _NYA_HTTP_PERMISSIONS.subject_of  = subject_of;
}

NYA_Permissions* nya_http_permissions(void) {
    return _NYA_HTTP_PERMISSIONS.permissions;
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

    // before every layer and the handler, so no route that writes can be reached from another site by forgetting.
    if (!nya_http_method_is_safe(exchange->request->method) && _nya_http_request_is_cross_site(exchange->request)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "a request from another site may not change anything here");
    }

    // Root layers outside the resource's own, both flattened into one array so the chain is a single index rather than two nested walks; bounded by construction, both halves checked at merge.
    NYA_HttpLayerFn chain_layers[NYA_HTTP_MAX_LAYERS * 2] = { 0 };
    u32             chain_count                           = 0;

    for (u32 index = 0; index < layer_count && index < NYA_HTTP_MAX_LAYERS; index++) {
        if (layers[index] == nullptr) continue;

        chain_layers[chain_count++] = layers[index];
    }

    for (u32 index = 0; index < router_count && index < NYA_HTTP_MAX_ROUTERS; index++) {
        const NYA_HttpRouter* router = routers[index];
        if (router == nullptr) continue;

        // the router the matched route came from, found by address: the route table is a contiguous array, so a pointer inside it identifies its owner without a back pointer per route.
        if (exchange->route < router->routes || exchange->route >= router->routes + router->route_count) continue;

        for (u32 layer = 0; layer < router->layer_count && layer < NYA_HTTP_MAX_LAYERS; layer++) {
            if (router->layers[layer] == nullptr) continue;

            chain_layers[chain_count++] = router->layers[layer];
        }

        break;
    }

    NYA_HttpChain chain = { .layers = chain_layers, .count = chain_count, .index = 0 };

    NYA_HttpStatus status = nya_http_chain_next(exchange, &chain);

    // The documented status list, enforced: a handler answering with something its route doesn't declare has made the OpenAPI document wrong, and with assertions on that's a test failure rather than something a schema reader finds out later.
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
        // the error body itself wouldn't fit or wouldn't serialize, our bug not the caller's; the status still goes out, there's just nothing to read with it.
        nya_log_error("The %d problem body could not be written.", (s32)status);
        nya_http_response_reset(exchange->response);
    }

    return status;
}

// PRIVATE API IMPLEMENTATION

/* How a callback token becomes a function, installed by the program a layer above; null until it is. */
NYA_INTERNAL NYA_HttpHandlerResolver    _nya_http_handler_resolver    = nullptr;
NYA_INTERNAL NYA_HttpIdentifiedResolver _nya_http_identified_resolver = nullptr;

void nya_http_router_resolvers_set(NYA_HttpHandlerResolver handler, NYA_HttpIdentifiedResolver identified) {
    _nya_http_handler_resolver    = handler;
    _nya_http_identified_resolver = identified;
}

NYA_HttpStatus _nya_http_router_run_route(NYA_HttpExchange* exchange) {
    const NYA_HttpRoute* route = exchange->route;

    nya_assert(route != nullptr, "the chain reached its innermost step with no route");

    if (route->auth == NYA_HTTP_AUTH_NONE) {
        // The token wins when set: it is re-resolved every dispatch, so a reloaded DLL's new handler runs.
        NYA_HttpHandlerFn handler = route->handler;

        if (route->handler_callback != 0) {
            nya_assert(_nya_http_handler_resolver != nullptr, "a route carries a handler_callback but no resolver was set; call nya_http_router_resolvers_set");
            handler = _nya_http_handler_resolver(route->handler_callback);
        }

        nya_assert(handler != nullptr, "nya_http_router_check refuses a route with no handler");

        return handler(exchange);
    }

    NYA_HttpStatus refused = _nya_http_router_extract_identity(exchange);
    if (refused != NYA_HTTP_STATUS_NONE) return refused;

    NYA_HttpIdentifiedFn handler = route->handler_identified;

    if (route->handler_identified_callback != 0) {
        nya_assert(_nya_http_identified_resolver != nullptr, "a route carries a handler_identified_callback but no resolver was set; call nya_http_router_resolvers_set");
        handler = _nya_http_identified_resolver(route->handler_identified_callback);
    }

    nya_assert(handler != nullptr, "nya_http_router_check refuses a route with no handler");
    nya_assert(exchange->identified, "the extractor returned no refusal and no identity");

    return handler(exchange, &exchange->identity);
}

NYA_HttpStatus _nya_http_router_extract_identity(NYA_HttpExchange* exchange) {
    const NYA_HttpRoute* route = exchange->route;

    exchange->identified = false;
    exchange->identity   = (NYA_HttpIdentity){ 0 };

    if (exchange->secret == nullptr || exchange->secret_size < NYA_HTTP_MIN_SECRET_BYTES) {
        // a route that needs a token on a server that can't verify one: not the caller's fault and not something they can fix by retrying with credentials.
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "this server was started without a signing secret");
    }

    const char* token      = nullptr;
    u64         token_size = 0;

    if (!nya_http_access_token(exchange->request, &token, &token_size)) {
        NYA_HttpStatus status = nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNAUTHORIZED, "this route needs a bearer token");

        // after the body, not before: writing the problem empties the response (headers included), so a layer replacing an answer can't leave half of the previous one behind.
        NYA_Error announced = nya_http_response_header(exchange->response, "WWW-Authenticate", "Bearer");
        if (!announced.ok) nya_log_warn("The WWW-Authenticate header could not be added to a 401.");

        return status;
    }

    NYA_Error verified =
        nya_http_jwt_decode(exchange->arena, token, token_size, exchange->secret, exchange->secret_size, exchange->now_s, &exchange->identity);

    if (!verified.ok) {
        // One answer for every way a token can be wrong: telling a caller their signature was fine but the token expired tells an attacker their forgery verified.
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

    // And what the program's own table says about whoever the token names, a different question from what the token claims: a revoked role applies next request, a scope applies when the token expires. Checked here so a handler can't be reached unchecked.
    if (route->permission != NYA_PERMISSION_NONE) {
        if (_NYA_HTTP_PERMISSIONS.permissions == nullptr || _NYA_HTTP_PERMISSIONS.subject_of == nullptr) {
            exchange->identity = (NYA_HttpIdentity){ 0 };

            // the same shape as a missing signing secret: the server can't answer the question this route asks, so it says so rather than letting the request through.
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "this route needs a permission table and none is installed");
        }

        u64 subject = _NYA_HTTP_PERMISSIONS.subject_of(&exchange->identity);

        // NYA_PERMISSION_SYSTEM answers yes to everything, so a token resolving to it is refused rather than obeyed: it's the program's own id, and nothing over a socket may borrow it.
        if (subject == NYA_PERMISSION_SYSTEM) {
            exchange->identity = (NYA_HttpIdentity){ 0 };

            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "that token names nobody this server knows");
        }

        u64 resource = route->resource_of != nullptr ? route->resource_of(exchange) : route->resource;

        if (!nya_permission_has(_NYA_HTTP_PERMISSIONS.permissions, subject, resource, route->permission)) {
            exchange->identity = (NYA_HttpIdentity){ 0 };

            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "that caller may not do this here");
        }
    }

    exchange->identified = true;

    return NYA_HTTP_STATUS_NONE;
}

b8 _nya_http_request_is_cross_site(const NYA_HttpRequest* request) {
    nya_assert(request != nullptr);

    // only "same-origin" and "none" (typed, bookmarked) are a page of this site or the user themselves.
    NYA_ConstCString fetch_site = nya_http_request_header(request, "Sec-Fetch-Site");
    if (fetch_site != nullptr && !nya_string_equals(fetch_site, "same-origin") && !nya_string_equals(fetch_site, "none")) return true;

    NYA_ConstCString origin = nya_http_request_header(request, "Origin");
    if (origin == nullptr) return false;

    // an Origin is a scheme, "://" and the host with its port, and nothing else; "null" and anything unparsed refuse.
    NYA_ConstCString authority = nullptr;
    if (nya_string_starts_with(origin, "http://")) authority = origin + strlen("http://");
    if (nya_string_starts_with(origin, "https://")) authority = origin + strlen("https://");
    if (authority == nullptr) return true;

    NYA_ConstCString host = nya_http_request_header(request, "Host");
    if (host == nullptr) return true;

    return !_nya_http_equals_ignore_case(authority, strlen(authority), host);
}

b8 _nya_http_route_declares(const NYA_HttpRoute* route, NYA_HttpStatus status) {
    for (u32 index = 0; index < NYA_HTTP_MAX_STATUSES && route->statuses[index] != NYA_HTTP_STATUS_NONE; index++) {
        if (route->statuses[index] == status) return true;
    }

    return false;
}
