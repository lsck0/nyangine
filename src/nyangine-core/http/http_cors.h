/**
 * @file http_cors.h
 *
 * Cross-Origin Resource Sharing, per route rather than for the whole server, and off unless a route
 * or its router asks for it. What tells a browser that a page from another origin may read this
 * route's answer, and nothing more: the same-origin default the browser already enforces stands
 * everywhere a policy does not reach.
 *
 * ```
 * nya_http_cors_check         whether a policy is well formed. Called for you by nya_http_router_check
 * nya_http_cors_allow_origin  the Origin this policy allows for a request, reflected back, or null
 * nya_http_cors_apply         adds the Access-Control-* headers to an actual response when allowed
 * nya_http_cors_preflight     answers an OPTIONS preflight, 204 with the negotiated headers and no body
 * ```
 *
 * ── never `*` with credentials ──
 *
 * A response that carries credentials — a cookie, an Authorization header — may name exactly one
 * origin in Access-Control-Allow-Origin and never the wildcard, because a browser refuses the pair and
 * a server that sent it would be handing every origin a credentialed reply. So a policy lists the
 * origins it trusts by their exact text and this module reflects back the request's own Origin when it
 * is one of them; a lone "*" entry is the "any origin" shorthand and is honoured only when credentials
 * are off, where there is nothing private to leak. A "*" beside credentials is not a runtime choice to
 * get wrong: nya_http_cors_check refuses the table at startup.
 *
 * ── default deny, bounded allowlist ──
 *
 * A route with no policy, or a policy whose origin list is empty, answers no Access-Control-* header at
 * all: the request is served (a cross-site read always was) but the browser hands the page nothing,
 * which is the same-origin default. The origin list is bounded (NYA_HTTP_CORS_MAX_ORIGINS) so a table
 * cannot ask the matcher to walk an unbounded array, and the whole policy is `static const` data beside
 * the route it belongs to.
 *
 * ── the cross-site write guard ──
 *
 * nya_http_router_dispatch refuses a write (POST, PUT, PATCH, DELETE) that came from another site, the
 * CSRF case. A CORS policy that allows the request's Origin is the program saying that cross-origin
 * caller is expected, so dispatch lets such a request through the guard: the allowlist is the grant.
 * A route with no policy keeps the guard, so nothing loses its default protection by omission.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_message.h"
#include "nyangine-core/http/http_types.h"

// CONSTANTS

/**
 * Origins one policy may list. A public API fronts a handful of first-party origins and their staging
 * twins; eight is past that and small enough that the matcher walks it per request without a thought.
 * A policy that lists more is refused by nya_http_cors_check rather than silently walked to the bound.
 * */
#define NYA_HTTP_CORS_MAX_ORIGINS 8

// TYPES

typedef struct NYA_HttpCors NYA_HttpCors;

/**
 * One route's CORS policy. Plain data, so it is a `static const` beside the route table and nothing
 * registers it at startup; a null pointer on a route is no policy, which is the default and denies.
 * */
struct NYA_HttpCors {
    /**
     * The origins allowed, each matched exactly against the request's Origin: "https://app.example.com",
     * scheme and host and any non-default port, lowercased as a browser sends it. The reflected value is
     * the request's own Origin, never a joined list, so a browser only ever sees the one it sent.
     *
     * A single "*" entry is "any origin" and is honoured only when `credentials` is false; see the file
     * note. Empty (or a null array) denies every origin, which is the safe default a zeroed policy is.
     * */
    const NYA_ConstCString* origins;
    u32                     origin_count;

    /**
     * The methods a preflight is told the route allows, listed in Access-Control-Allow-Methods. The
     * preflight is granted only when the caller's Access-Control-Request-Method is one of these; an
     * empty list allows none, so a policy that means to be used names at least the route's own verb.
     * */
    const NYA_HttpMethod* methods;
    u32                   method_count;

    /**
     * The request headers a preflight is told the route allows, in Access-Control-Allow-Headers:
     * "Authorization", "Content-Type". A browser only sends a header a preflight cleared, so a route
     * reading a custom request header names it here or never sees it cross-origin.
     * */
    const NYA_ConstCString* headers;
    u32                     header_count;

    /**
     * The response headers a browser is allowed to read, in Access-Control-Expose-Headers. Without it a
     * cross-origin caller reads only the handful of headers the fetch spec exposes by default, so a
     * route whose answer a client reads a custom header off names it here.
     * */
    const NYA_ConstCString* expose;
    u32                     expose_count;

    /** Seconds a browser may cache the preflight, in Access-Control-Max-Age. Zero omits the header. */
    u32 max_age_s;

    /**
     * Whether the browser may send credentials with the request and read the credentialed reply, which
     * puts Access-Control-Allow-Credentials: true on the answer. It is refused beside a "*" origin; see
     * the file note. A route behind the identity extractor that a browser calls with a cookie sets it.
     * */
    b8 credentials;
};

// FUNCTIONS

/**
 * Whether `cors` is a policy this server will serve, and an error naming the fault when it is not: an
 * origin list past NYA_HTTP_CORS_MAX_ORIGINS, a count that disagrees with a null array, or the one
 * combination that is a vulnerability rather than a preference — a "*" origin with credentials on.
 *
 * Null is a route with no policy and passes: default-deny is well formed. Called by nya_http_router_check.
 * */
NYA_API NYA_Error nya_http_cors_check(const NYA_HttpCors* cors) __attr_no_discard;

/**
 * The value to reflect in Access-Control-Allow-Origin for `request` under `cors`, or null when the
 * request is not allowed a CORS answer: no policy, no Origin header, or an Origin that is not on the
 * allowlist. An exact match returns the request's own Origin; a "*" entry with credentials off returns
 * "*". Never returns "*" when credentials are on, and never an origin the list does not carry.
 * */
NYA_API NYA_ConstCString nya_http_cors_allow_origin(const NYA_HttpCors* cors, const NYA_HttpRequest* request) __attr_no_discard;

/**
 * Adds the CORS headers to an actual (non-preflight) response when the request's Origin is allowed:
 * Access-Control-Allow-Origin, Vary: Origin whenever a specific origin was reflected (so a shared cache
 * keeps origins apart), Access-Control-Allow-Credentials when the policy carries them, and
 * Access-Control-Expose-Headers when it lists any. A disallowed or absent Origin adds nothing.
 *
 * Best effort against the response's header budget: a header that will not fit is logged and skipped
 * rather than failing the exchange, since the answer is still correct, only unreadable cross-origin.
 * */
NYA_API void nya_http_cors_apply(const NYA_HttpCors* cors, const NYA_HttpRequest* request, NYA_HttpResponse* response);

/**
 * Answers a CORS preflight: empties `response`, and when the Origin is allowed and the requested method
 * (Access-Control-Request-Method) is one the policy allows, writes the negotiated Access-Control-*
 * headers — allow-origin, allow-methods, allow-headers, max-age and, when set, allow-credentials — and
 * Vary: Origin. A disallowed origin or method leaves the headers off, which the browser reads as a
 * refusal. Always 204 with no body, whichever way it went; a preflight carries no content.
 * */
NYA_API NYA_HttpStatus nya_http_cors_preflight(const NYA_HttpCors* cors, const NYA_HttpRequest* request, NYA_HttpResponse* response);
