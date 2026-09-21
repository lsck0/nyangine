/**
 * @file http_openapi.h
 *
 * The schema, generated from the route tables and the DTO reflections, and served by the program it
 * describes. Never written by hand and never stored: it is built from the same data the dispatcher
 * routes through, so a document that disagrees with the code cannot exist.
 *
 * ```
 * nya_http_openapi_router     the routes that serve it. Merge it and the schema is online
 * nya_http_openapi_document   the OpenAPI 3.1 document, as JSON
 * nya_http_openapi_page       a page listing every route, generated from the same walk
 * nya_http_openapi_schema     one DTO's JSON Schema, from its NYA_TypeReflection
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()));
 * // GET /openapi.json  the document
 * // GET /docs          the page
 * ```
 *
 * ── where each part comes from ──
 *
 * ```
 * paths, methods        NYA_HttpRoute.path and .method
 * summary, description  NYA_HttpRoute.summary and .description
 * tags                  NYA_HttpRouter.name, so one resource is one group
 * security              NYA_HttpRoute.auth and .scope
 * responses             NYA_HttpRoute.statuses, with the reason phrase as the description
 * requestBody           nya_http_openapi_schema(NYA_HttpRoute.request_type)
 * schemas               nya_http_openapi_schema of every type any route names
 * ```
 *
 * Nothing above is a string this file holds. A route that grows a status, a DTO that grows a field,
 * a resource that is mounted or unmounted: each of them changes the document and none of them is a
 * second edit.
 *
 * ── the schema mirrors the serializer, not the C type ──
 *
 * A DTO goes out through nya_reflect_to_object, which writes an enum as its variant *name*, a set of
 * bitflags as a list of names, and a `char[N]` as text. The schema says the same, because a schema
 * describing the C layout rather than the bytes on the wire would be wrong in exactly the places a
 * generated client would trust it.
 *
 * ── the page ──
 *
 * Generated in C from the route table, like docs/CHEATSHEET.md is generated from the headers. It
 * loads nothing from a CDN, works offline, and is not a document anybody maintains. It is deliberately
 * plain: the real browsable interface arrives when the UI backend can emit HTML from `nya_ui_*` calls,
 * and this is the placeholder that does not need throwing away first. See TODO.md, "Web".
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_string.h"
#include "nyangine/http/http_router.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_HTTP_OPENAPI_PATH "/openapi.json"
#define NYA_HTTP_DOCS_PATH    "/docs"

/** The version of the OpenAPI specification the document claims. */
#define NYA_HTTP_OPENAPI_VERSION "3.1.0"

/**
 * How deep nya_http_openapi_schema follows nested types.
 *
 * A reflection graph is acyclic by construction, since a type cannot contain itself by value, but a
 * bound is cheaper than trusting that and is what keeps a generated document from being unbounded
 * work.
 * */
#define NYA_HTTP_OPENAPI_MAX_DEPTH 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The routes that serve the document and the page. A router like any other; merge it or do not.
 *
 * Both are open: a schema is the shape of an API and not a secret, and a server that needs its schema
 * authenticated has bigger problems than this router. Static storage, so it outlives any mount.
 * */
NYA_API const NYA_HttpRouter* nya_http_openapi_router(void) __attr_no_discard;

/**
 * Builds the document for everything mounted on the server right now, allocated from `arena`.
 *
 * NYA_ERROR_NOT_FOUND when the server is not running, since there is nothing to describe.
 * */
NYA_API NYA_Error nya_http_openapi_document(NYA_Arena* arena, OUT NYA_String** out_json) __attr_no_discard;

/** The same walk, rendered as a page. Allocated from `arena`. */
NYA_API NYA_Error nya_http_openapi_page(NYA_Arena* arena, OUT NYA_String** out_html) __attr_no_discard;

/**
 * One type's JSON Schema, as the serializer would write it. Null for a type reflection that describes
 * nothing a document can carry, which is a pointer to anything but `char`.
 * */
NYA_API NYA_Object* nya_http_openapi_schema(NYA_Arena* arena, const NYA_TypeReflection* type) __attr_no_discard;
