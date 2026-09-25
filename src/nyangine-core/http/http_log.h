/**
 * @file http_log.h
 *
 * What one exchange leaves behind, and what it is not allowed to leave behind.
 *
 * ```
 * nya_http_layer_log          the layer: one record per request, at the configured level
 * nya_http_log_config_set     the level, the address policy and the extra header deny list
 * nya_http_log_config_get     what is in force
 * nya_http_log_header_is_denied  whether a header name is redacted, deny list and config together
 * ```
 *
 * ── the levels ──
 *
 * | Level      | What the record carries                                                          |
 * | :--------- | :------------------------------------------------------------------------------- |
 * | `summary`  | method, route, status, duration, bytes in and out, caller, address. The default    |
 * | `headers`  | that, plus every request and response header and the query string                  |
 * | `bodies`   | that, plus both bodies, decoded through the route's DTO and capped                  |
 *
 * Set per server in `engine.nya` under `engine.http_log`, and applied again on every hot reload,
 * because NYA_HttpLogConfig carries `@on_apply`. A program with no config file calls
 * nya_http_log_config_set itself.
 *
 * ── redaction happens here, once, before any sink ──
 *
 * The engine has one log entry point and it fans out to every sink there is: the file, the terminal,
 * the ring a crash report prints, and whatever else a program registered. A sink cannot filter what it
 * was handed without every other sink having been handed it too, so filtering in a sink is not
 * filtering at all. The record below is therefore assembled redacted and only then logged, and there is
 * no intermediate form of it holding anything a sink may not see.
 *
 * Three rules, in the order a record meets them:
 *
 * - **Headers on a deny list are never written**, whatever the level: `Authorization`, `Cookie`,
 *   `Set-Cookie` and `Proxy-Authorization`, plus whatever `deny` names, which is where an API key
 *   header goes.
 * - **Bodies are redacted structurally**, through the route's own `request_type` and `response_type`
 *   reflection: a field tagged `@redact` is NYA_REFLECT_REDACTED at any depth, whatever it is spelled
 *   and whatever it holds. Not a regex over the bytes, which would be a second description of the type
 *   that nothing keeps in step with the first.
 * - **Query parameters are redacted by name**: a name a `@redact` field of the route's request type
 *   carries, a name `deny` lists, or a name containing `password`, `token`, `secret` or `code`. A query
 *   string has no schema, so the name is all there is to go on, and those four words are exactly what
 *   `./build check` refuses an untagged reflected field for.
 *
 * ── fail closed ──
 *
 * A body that does not parse as its route's DTO is logged as its size and the first eight bytes of its
 * BLAKE2b, never as bytes. That covers every interesting case at once: a route with no DTO, a body in a
 * format the route did not declare, a body with a key the type has no field for, and a body that is not
 * a document at all. An unparsed body is exactly where an unredacted secret would be hiding, so the one
 * thing never done with it is print it. The hash is still enough to say "the same body arrived twice"
 * and to match a record against a capture taken elsewhere.
 *
 * A binary `application/nya` body is decoded through the same DTO and logged as text, so the native
 * format is as readable in a log as JSON and no less redacted.
 *
 * ── what the record does not see ──
 *
 * This is a layer, so it runs inside nya_http_router_dispatch and returns before it does. A refusal
 * body the dispatcher writes after the chain has unwound — the NYA_HttpProblem behind every 4xx that a
 * handler returned bare — is therefore not in the record, and the response byte count is the count at
 * the moment the chain returned. That is a gap in what the record says, never a leak: what is missing
 * is this server's own words, and no part of it is the caller's.
 *
 * ── privacy ──
 *
 * A request log carries the caller's network and not the caller: `/24` for IPv4 and `/48` for IPv6, by
 * nya_http_address_truncate. `full` and `none` are the other two settings, and `full` is what a machine
 * handling abuse reports wants. A security event is a different record with a different retention and
 * keeps the address whole; that is not this layer's business.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-core/http/http_types.h"

// CONSTANTS

/**
 * Bytes of one exchange's record, terminator included.
 *
 * NYA_LOG_MESSAGE_MAX_LENGTH, because the record is logged in one call and a line longer than that is
 * cut by the formatter anyway; the static assert in http_log.c holds the two together. Everything past
 * it is dropped with a "..." rather than silently, so a reader can tell a short record from a cut one.
 * */
#define NYA_HTTP_LOG_MAX_RECORD_BYTES 2048

/**
 * Bytes of one body the record carries, before the "..." .
 *
 * Written down here rather than left to whatever fits: a body is up to NYA_HTTP_MAX_BODY_BYTES and a
 * record holds two of them plus the headers, so an uncapped body would push everything after it out of
 * the record. Five hundred and twelve is every DTO in this tree several times over.
 * */
#define NYA_HTTP_LOG_MAX_BODY_BYTES 512

/**
 * Bytes of one header or query value the record carries. A value is a hint about what arrived, not the
 * thing itself, and one hostile header must not be able to take the whole record.
 * */
#define NYA_HTTP_LOG_MAX_VALUE_BYTES 128

/** Bytes of the configurable deny list, terminator included: a handful of header names, comma separated. */
#define NYA_HTTP_LOG_MAX_DENY_BYTES 256

/** Hex digits of the body hash a fail-closed record carries. Sixty four bits of BLAKE2b; see http_static.h for the same choice. */
#define NYA_HTTP_LOG_HASH_DIGITS 16

// TYPES

typedef enum NYA_HttpLogLevel     NYA_HttpLogLevel;
typedef enum NYA_HttpLogAddress   NYA_HttpLogAddress;
typedef struct NYA_HttpLogConfig  NYA_HttpLogConfig;

// @reflect
/** How much of an exchange the record carries. Ordered, so a level includes everything below it. */
enum NYA_HttpLogLevel {
    /** Method, route, status, duration, bytes in and out, caller and address. The default. */
    NYA_HTTP_LOG_SUMMARY = 0,

    /** And every header, denied ones redacted, and the query string, secret names redacted. */
    NYA_HTTP_LOG_HEADERS,

    /** And both bodies, through the route's DTOs, capped at NYA_HTTP_LOG_MAX_BODY_BYTES. */
    NYA_HTTP_LOG_BODIES,

    NYA_HTTP_LOG_LEVEL_COUNT,
};

// @reflect
/** How much of the caller's address the record carries. */
enum NYA_HttpLogAddress {
    /** The network and not the host: 203.0.113.7 as 203.0.113.0/24. The default. */
    NYA_HTTP_LOG_ADDRESS_NETWORK = 0,

    /** All of it. What a machine that has to answer an abuse report wants, and a decision to make on purpose. */
    NYA_HTTP_LOG_ADDRESS_FULL,

    /** None of it. */
    NYA_HTTP_LOG_ADDRESS_NONE,

    NYA_HTTP_LOG_ADDRESS_COUNT,
};

// @reflect @on_apply(_nya_http_log_config_apply)
/**
 * What this server logs. Zero is the default everywhere, so a program that sets nothing gets the
 * summary line and truncated addresses.
 *
 * `@on_apply` is what makes a reload of `engine.nya` reach the running server: reflection writes the
 * fields and then hands the struct to the layer, so there is no apply call for a program to forget.
 * */
struct NYA_HttpLogConfig {
    NYA_HttpLogLevel level;

    NYA_HttpLogAddress address;

    /**
     * Header names redacted beyond the four that always are, comma separated and matched without
     * regard to case: `"x-api-key,x-hub-signature"`.
     *
     * A list rather than a pattern, because a pattern that is wrong fails open and a name that is
     * missing fails visibly. The same names redact a query parameter.
     * */
    char deny[NYA_HTTP_LOG_MAX_DENY_BYTES];
};

// FUNCTIONS

/** Installs `config`. Copied, so the caller's struct need not outlive the call. */
NYA_API void nya_http_log_config_set(NYA_HttpLogConfig config);

/** What is in force. */
NYA_API NYA_HttpLogConfig nya_http_log_config_get(void) __attr_no_discard;

/**
 * Whether a header called `name` is redacted: one of the four the engine always redacts, or one the
 * configured deny list names. Matched without regard to case, as header names are.
 *
 * Public because it is the rule itself, and a second implementation of it anywhere — in a program's own
 * layer, in a test — would be a second answer.
 * */
NYA_API b8 nya_http_log_header_is_denied(NYA_ConstCString name) __attr_no_discard;

/**
 * One record per request, at the configured level, redacted before it is logged.
 *
 * Outermost when a program installs it, so the duration is the whole exchange and the status is
 * whatever anything inside decided. The request id arrives through the server's log tag rather than
 * through this line.
 * */
NYA_API NYA_HttpStatus nya_http_layer_log(NYA_HttpExchange* exchange, NYA_HttpChain* next);

// INTERNALS

/** What `@on_apply` on NYA_HttpLogConfig names. Named in the generated reflection table, not by hand. */
NYA_API NYA_Error _nya_http_log_config_apply(void* instance);
