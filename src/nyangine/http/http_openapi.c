#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_version.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/http/http_openapi.h"
#include "nyangine/http/http_server.h"
#include "nyangine/serde/serde.h"

// PRIVATE API DECLARATION

/** An NYA_Value holding `text`, which the caller has to keep alive for as long as the document. */
NYA_INTERNAL NYA_Value _nya_http_json_string(NYA_ConstCString text) __attr_no_discard;

/** An NYA_Value holding `object`, by value as NYA_Value requires. */
NYA_INTERNAL NYA_Value _nya_http_json_object(const NYA_Object* object) __attr_no_discard;

/** The JSON Schema for one type, at `depth` of NYA_HTTP_OPENAPI_MAX_DEPTH. */
NYA_INTERNAL NYA_Object* _nya_http_schema_of(NYA_Arena* arena, const NYA_TypeReflection* type, u32 depth) __attr_no_discard;

/** The schema for one field, which is its type's unless a hint says the field means something else. */
NYA_INTERNAL NYA_Object* _nya_http_schema_of_field(NYA_Arena* arena, const NYA_ReflectField* field, u32 depth) __attr_no_discard;

/** {"type":"string","enum":[names]}, or a list of those when the enum is a set of flags. */
NYA_INTERNAL NYA_Object* _nya_http_schema_of_enum(NYA_Arena* arena, const NYA_TypeReflection* type, b8 bitflags) __attr_no_discard;

/** What JSON calls one of NYA_Type's primitives, and what OpenAPI calls its width. */
NYA_INTERNAL void _nya_http_schema_of_primitive(NYA_Object* schema, NYA_Type primitive);

/** The "responses" object for one route, from its declared status list. */
NYA_INTERNAL NYA_Object* _nya_http_responses_of(NYA_Arena* arena, const NYA_HttpRoute* route) __attr_no_discard;

/** The "operation" object for one route. */
NYA_INTERNAL NYA_Object* _nya_http_operation_of(NYA_Arena* arena, const NYA_HttpRouter* router, const NYA_HttpRoute* route) __attr_no_discard;

/** Appends `text` with `<`, `>` and `&` replaced, so a summary cannot become markup. */
NYA_INTERNAL void _nya_http_page_escape(NYA_String* html, NYA_ConstCString text);

/* The two routes this module serves. */

NYA_INTERNAL NYA_HttpStatus _nya_http_openapi_get(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_docs_get(NYA_HttpExchange* exchange);

// CONSTANTS

// The two GET routes in a module whose resources are otherwise QUERY, POST, PUT and DELETE: a browser opening /docs and a generator fetching /openapi.json both send GET with no parameters, the only thing QUERY buys; see http_openapi.h.
NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_OPENAPI_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_GET,
     .path        = NYA_HTTP_OPENAPI_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = _nya_http_openapi_get,
     .summary     = "The OpenAPI document for everything this program serves",
     .description = "Generated from the mounted route tables and the DTO reflections on every request, so it describes "
                       "exactly what is mounted right now.", .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method      = NYA_HTTP_METHOD_GET,
     .path        = NYA_HTTP_DOCS_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = _nya_http_docs_get,
     .summary     = "Every route, as a page",
     .description = "The same walk as the document, rendered for a person rather than for a generator.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_OPENAPI_ROUTER = {
    .name        = "schema",
    .routes      = _NYA_HTTP_OPENAPI_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_OPENAPI_ROUTES),
};

// PUBLIC API IMPLEMENTATION

const NYA_HttpRouter* nya_http_openapi_router(void) {
    return &_NYA_HTTP_OPENAPI_ROUTER;
}

NYA_Object* nya_http_openapi_schema(NYA_Arena* arena, const NYA_TypeReflection* type) {
    return _nya_http_schema_of(arena, type, 0);
}

NYA_Error nya_http_openapi_document(NYA_Arena* arena, NYA_String** out_json) {
    nya_assert(arena != nullptr);
    nya_assert(out_json != nullptr);

    *out_json = nullptr;

    u32 router_count = nya_http_server_router_count();
    if (router_count == 0) return nya_error(NYA_ERROR_NOT_FOUND, "nothing is mounted, so there is nothing to describe");

    NYA_Object* document = nya_object_create(arena);

    nya_object_add(document, "openapi", _nya_http_json_string(NYA_HTTP_OPENAPI_VERSION));

    NYA_Object* info = nya_object_create(arena);
    nya_object_add(info, "title", _nya_http_json_string("nyangine"));
    nya_object_add(info, "version", _nya_http_json_string(NYA_VERSION));
    nya_object_add(info, "description", _nya_http_json_string("Generated from the route tables of a running nyangine program."));
    nya_object_add(document, "info", _nya_http_json_object(info));

    // One tag per mounted resource, in mount order, so a reader sees the same grouping the code has.
    NYA_ArrayᐸNYA_Valueᐳ* tags = nya_array_create(arena, NYA_Value);

    NYA_Object* paths   = nya_object_create(arena);
    NYA_Object* schemas = nya_object_create(arena);

    for (u32 index = 0; index < router_count; index++) {
        const NYA_HttpRouter* router = nya_http_server_router_at(index);
        if (router == nullptr) continue;

        NYA_Object* tag = nya_object_create(arena);
        nya_object_add(tag, "name", _nya_http_json_string(router->name));
        nya_array_push_back(tags, _nya_http_json_object(tag));

        for (u32 position = 0; position < router->route_count; position++) {
            const NYA_HttpRoute* route = &router->routes[position];

            NYA_Object* operation = _nya_http_operation_of(arena, router, route);
            if (operation == nullptr) continue;

            // OpenAPI keys a path by its methods, so two routes on one path share an entry: looked up rather than replaced, which lets a GET and a POST on the same path both appear.
            NYA_Value* existing = nya_object_get(paths, (NYA_CString)route->path);

            NYA_Object* item = nullptr;

            if (existing != nullptr && existing->type == NYA_TYPE_OBJECT) {
                item = &existing->as_object;
            } else {
                item = nya_object_create(arena);
            }

            // Lowercase, as the spec requires of a method key; "query" is a Path Item Object field as of OpenAPI 3.2.0 and nothing before it, which is why NYA_HTTP_OPENAPI_VERSION says 3.2.0 (reasoning in http_openapi.h).
            char method[16] = { 0 };

            NYA_ConstCString text = nya_http_method_text(route->method);
            for (u32 character = 0; character + 1 < sizeof(method) && text[character] != '\0'; character++) {
                method[character] = _nya_http_lower(text[character]);
            }

            NYA_CString key = nya_string_to_cstring(arena, nya_string_from(arena, method));

            nya_object_add(item, key, _nya_http_json_object(operation));

            if (existing == nullptr || existing->type != NYA_TYPE_OBJECT) {
                nya_object_add(paths, (NYA_CString)route->path, _nya_http_json_object(item));
            }

            // every DTO any route names, once, under its own C type name.
            const NYA_TypeReflection* named[2] = { route->request_type, route->response_type };

            for (u32 slot = 0; slot < 2; slot++) {
                if (named[slot] == nullptr) continue;
                if (nya_object_get(schemas, (NYA_CString)named[slot]->name) != nullptr) continue;

                NYA_Object* schema = _nya_http_schema_of(arena, named[slot], 0);
                if (schema == nullptr) continue;

                nya_object_add(schemas, (NYA_CString)named[slot]->name, _nya_http_json_object(schema));
            }
        }
    }

    // the error body is on every route that can refuse, which is all of them, so it's named once here rather than by each.
    if (nya_object_get(schemas, "NYA_HttpProblem") == nullptr) {
        NYA_Object* problem = _nya_http_schema_of(arena, nya_reflect_of(NYA_HttpProblem), 0);

        if (problem != nullptr) nya_object_add(schemas, "NYA_HttpProblem", _nya_http_json_object(problem));
    }

    nya_object_add(document, "tags", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *tags });
    nya_object_add(document, "paths", _nya_http_json_object(paths));

    // One security scheme, because there is one: a bearer JWT, named here and referenced by every operation that needs it, which is how a generated client knows to send the header.
    NYA_Object* bearer = nya_object_create(arena);
    nya_object_add(bearer, "type", _nya_http_json_string("http"));
    nya_object_add(bearer, "scheme", _nya_http_json_string("bearer"));
    nya_object_add(bearer, "bearerFormat", _nya_http_json_string("JWT"));

    NYA_Object* security_schemes = nya_object_create(arena);
    nya_object_add(security_schemes, "bearer", _nya_http_json_object(bearer));

    NYA_Object* components = nya_object_create(arena);
    nya_object_add(components, "securitySchemes", _nya_http_json_object(security_schemes));
    nya_object_add(components, "schemas", _nya_http_json_object(schemas));

    nya_object_add(document, "components", _nya_http_json_object(components));

    NYA_String* json = nya_serialize(arena, document, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    if (json == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the OpenAPI document could not be serialized");

    *out_json = json;

    return NYA_OK;
}

/**
 * The page's one style block, kept apart so the Content-Security-Policy the page is served with can name it by its
 * hash: the page loads nothing else, and the server's default CSP would otherwise refuse even this.
 * */
#define _NYA_HTTP_PAGE_STYLE                                                                                                                     \
    "body{font:14px ui-monospace,monospace;margin:2rem;max-width:60rem}"                                                                             \
    "h2{margin-top:2rem}table{border-collapse:collapse;width:100%}"                                                                                  \
    "td,th{border-bottom:1px solid #ccc;padding:.4rem;text-align:left;vertical-align:top}"                                                          \
    "code{background:#f2f2f2;padding:.1rem .3rem}"

NYA_Error nya_http_openapi_page(NYA_Arena* arena, NYA_String** out_html) {
    nya_assert(arena != nullptr);
    nya_assert(out_html != nullptr);

    *out_html = nullptr;

    u32 router_count = nya_http_server_router_count();
    if (router_count == 0) return nya_error(NYA_ERROR_NOT_FOUND, "nothing is mounted, so there is nothing to describe");

    NYA_String* html = nya_string_create(arena);

    nya_string_extend(html, "<!doctype html><meta charset=utf-8><title>nyangine</title>");
    nya_string_extend(html, "<style>" _NYA_HTTP_PAGE_STYLE "</style>");

    nya_string_extend_sprintf(
        html,
        "<h1>nyangine %s</h1><p>Generated from the mounted route tables. The machine readable "
        "version is at <a href=\"%s\"><code>%s</code></a>.</p>",
        NYA_VERSION,
        NYA_HTTP_OPENAPI_PATH,
        NYA_HTTP_OPENAPI_PATH
    );

    for (u32 index = 0; index < router_count; index++) {
        const NYA_HttpRouter* router = nya_http_server_router_at(index);
        if (router == nullptr) continue;

        nya_string_extend(html, "<h2>");
        _nya_http_page_escape(html, router->name);
        nya_string_extend(html, "</h2><table><tr><th>route<th>auth<th>answers<th>what it does</tr>");

        for (u32 position = 0; position < router->route_count; position++) {
            const NYA_HttpRoute* route = &router->routes[position];

            nya_string_extend(html, "<tr><td><code>");
            _nya_http_page_escape(html, nya_http_method_text(route->method));
            nya_string_extend(html, " ");
            _nya_http_page_escape(html, route->path);
            nya_string_extend(html, "</code><td>");

            if (route->auth == NYA_HTTP_AUTH_NONE) {
                nya_string_extend(html, "open");
            } else {
                nya_string_extend(html, "bearer");

                const NYA_TypeReflection* scope = nya_reflect_of(NYA_HttpScope);

                for (u32 variant = 0; variant < scope->variant_count; variant++) {
                    if (scope->variants[variant].value == 0) continue;
                    if (((s64)route->scope & scope->variants[variant].value) != scope->variants[variant].value) continue;

                    nya_string_extend(html, "<br>");
                    _nya_http_page_escape(html, scope->variants[variant].name);
                }
            }

            nya_string_extend(html, "<td>");

            for (u32 status = 0; status < NYA_HTTP_MAX_STATUSES && route->statuses[status] != NYA_HTTP_STATUS_NONE; status++) {
                if (status > 0) nya_string_extend(html, "<br>");

                nya_string_extend_sprintf(html, "%d ", (s32)route->statuses[status]);
                _nya_http_page_escape(html, nya_http_status_text(route->statuses[status]));
            }

            nya_string_extend(html, "<td>");
            _nya_http_page_escape(html, route->summary);

            if (route->description != nullptr) {
                nya_string_extend(html, "<br>");
                _nya_http_page_escape(html, route->description);
            }

            if (route->request_type != nullptr) {
                nya_string_extend(html, "<br>body: <code>");
                _nya_http_page_escape(html, route->request_type->name);
                nya_string_extend(html, "</code>");
            }

            if (route->response_type != nullptr) {
                nya_string_extend(html, "<br>answers: <code>");
                _nya_http_page_escape(html, route->response_type->name);
                nya_string_extend(html, "</code>");
            }

            nya_string_extend(html, "</tr>");
        }

        nya_string_extend(html, "</table>");
    }

    *out_html = html;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

NYA_HttpStatus _nya_http_openapi_get(NYA_HttpExchange* exchange) {
    NYA_String* json = nullptr;

    NYA_Error built = nya_http_openapi_document(exchange->arena, &json);

    if (!built.ok) {
        nya_log_error("The OpenAPI document could not be built.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Error written = nya_http_response_bytes(exchange->response, (const u8*)json->items, json->length, NYA_HTTP_MEDIA_JSON);

    if (!written.ok) {
        nya_log_error("The OpenAPI document is larger than the response buffer.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_docs_get(NYA_HttpExchange* exchange) {
    NYA_String* html = nullptr;

    NYA_Error built = nya_http_openapi_page(exchange->arena, &html);

    if (!built.ok) {
        nya_log_error("The documentation page could not be built.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Error written = nya_http_response_bytes(exchange->response, (const u8*)html->items, html->length, NYA_HTTP_MEDIA_HTML);

    // the default policy with the page's own style block allowed by its hash, and nothing else loosened.
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256((const u8*)_NYA_HTTP_PAGE_STYLE, sizeof(_NYA_HTTP_PAGE_STYLE) - 1, &digest);

    NYA_String* hash = nya_string_create(exchange->arena);
    nya_base64_encode(hash, digest.bytes, sizeof(digest.bytes));

    NYA_String* policy = nya_string_sprintf(exchange->arena, "default-src 'none'; style-src 'sha256-%.*s'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
                                            (int)hash->length, hash->items);
    if (written.ok) written = nya_http_response_header(exchange->response, "Content-Security-Policy", nya_string_to_cstring(exchange->arena, policy));

    if (!written.ok) {
        nya_log_error("The documentation page is larger than the response buffer.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_Value _nya_http_json_string(NYA_ConstCString text) {
    return (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)text };
}

NYA_Value _nya_http_json_object(const NYA_Object* object) {
    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

NYA_Object* _nya_http_schema_of(NYA_Arena* arena, const NYA_TypeReflection* type, u32 depth) {
    if (type == nullptr || depth >= NYA_HTTP_OPENAPI_MAX_DEPTH) return nullptr;

    NYA_Object* schema = nya_object_create(arena);

    switch (type->kind) {
        case NYA_REFLECT_PRIMITIVE: _nya_http_schema_of_primitive(schema, type->primitive); return schema;

        case NYA_REFLECT_ENUM:      return _nya_http_schema_of_enum(arena, type, type->is_bitflags);

        case NYA_REFLECT_STRUCT:
        case NYA_REFLECT_UNION:     {
            NYA_Object* properties = nya_object_create(arena);

            // Every field is required: a DTO is a C struct with no absent fields, and nya_reflect_to_object writes all of them, so a schema calling any optional would describe an answer this server never sends.
            NYA_ArrayᐸNYA_Valueᐳ* required = nya_array_create(arena, NYA_Value);

            for (u32 index = 0; index < type->field_count; index++) {
                const NYA_ReflectField* field = &type->fields[index];

                NYA_Object* property = _nya_http_schema_of_field(arena, field, depth + 1);
                if (property == nullptr) continue;

                nya_object_add(properties, (NYA_CString)field->name, _nya_http_json_object(property));
                nya_array_push_back(required, _nya_http_json_string(field->name));
            }

            nya_object_add(schema, "type", _nya_http_json_string("object"));
            nya_object_add(schema, "properties", _nya_http_json_object(properties));
            nya_object_add(schema, "required", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *required });

            return schema;
        }

        case NYA_REFLECT_ARRAY:
        case NYA_REFLECT_VECTOR: {
            // a char array goes out as text, so it is described as text. See the header note.
            if (type->element != nullptr && type->element->kind == NYA_REFLECT_PRIMITIVE && type->element->primitive == NYA_TYPE_CHAR) {
                nya_object_add(schema, "type", _nya_http_json_string("string"));
                nya_object_add(schema, "maxLength", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)type->element_count - 1 });

                return schema;
            }

            NYA_Object* items = _nya_http_schema_of(arena, type->element, depth + 1);
            if (items == nullptr) return nullptr;

            nya_object_add(schema, "type", _nya_http_json_string("array"));
            nya_object_add(schema, "items", _nya_http_json_object(items));
            nya_object_add(schema, "minItems", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)type->element_count });
            nya_object_add(schema, "maxItems", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)type->element_count });

            return schema;
        }

        case NYA_REFLECT_POINTER: {
            // `char*` is a string and every other pointer is an address in one run of one process, nothing a document can carry. See base_reflection.h.
            if (type->element == nullptr || type->element->kind != NYA_REFLECT_PRIMITIVE || type->element->primitive != NYA_TYPE_CHAR) return nullptr;

            nya_object_add(schema, "type", _nya_http_json_string("string"));

            return schema;
        }

        case NYA_REFLECT_COUNT:
        default:                return nullptr;
    }
}

NYA_Object* _nya_http_schema_of_field(NYA_Arena* arena, const NYA_ReflectField* field, u32 depth) {
    // An integer with @flags(SomeEnum) is written as a list of names, like a bitflags enum, so it's described as one; the field's own type is an integer and says nothing about that.
    if (field->hint == NYA_HINT_BITFLAGS && field->type != nullptr && field->type->element != nullptr) {
        return _nya_http_schema_of_enum(arena, field->type->element, true);
    }

    return _nya_http_schema_of(arena, field->type, depth);
}

NYA_Object* _nya_http_schema_of_enum(NYA_Arena* arena, const NYA_TypeReflection* type, b8 bitflags) {
    if (type == nullptr || type->kind != NYA_REFLECT_ENUM) return nullptr;

    NYA_Object*           names    = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ* variants = nya_array_create(arena, NYA_Value);

    for (u32 index = 0; index < type->variant_count; index++) {
        // a zero flag is "none" and never appears in a list of set flags, so it's not a value the list form can hold.
        if (bitflags && type->variants[index].value == 0) continue;

        nya_array_push_back(variants, _nya_http_json_string(type->variants[index].name));
    }

    nya_object_add(names, "type", _nya_http_json_string("string"));
    nya_object_add(names, "enum", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *variants });

    if (!bitflags) return names;

    NYA_Object* schema = nya_object_create(arena);
    nya_object_add(schema, "type", _nya_http_json_string("array"));
    nya_object_add(schema, "items", _nya_http_json_object(names));

    return schema;
}

void _nya_http_schema_of_primitive(NYA_Object* schema, NYA_Type primitive) {
    switch (primitive) {
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: nya_object_add(schema, "type", _nya_http_json_string("boolean")); return;

        case NYA_TYPE_F16:
        case NYA_TYPE_F32:
            nya_object_add(schema, "type", _nya_http_json_string("number"));
            nya_object_add(schema, "format", _nya_http_json_string("float"));
            return;

        case NYA_TYPE_F64:
        case NYA_TYPE_F128:
            nya_object_add(schema, "type", _nya_http_json_string("number"));
            nya_object_add(schema, "format", _nya_http_json_string("double"));
            return;

        case NYA_TYPE_CHAR:
        case NYA_TYPE_WCHAR:
        case NYA_TYPE_STRING:
        case NYA_TYPE_WSTRING: nya_object_add(schema, "type", _nya_http_json_string("string")); return;

        case NYA_TYPE_U8:
        case NYA_TYPE_U16:
        case NYA_TYPE_U32:
            nya_object_add(schema, "type", _nya_http_json_string("integer"));
            nya_object_add(schema, "format", _nya_http_json_string("int32"));
            nya_object_add(schema, "minimum", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 0 });
            return;

        case NYA_TYPE_U64:
        case NYA_TYPE_U128:
            nya_object_add(schema, "type", _nya_http_json_string("integer"));
            nya_object_add(schema, "format", _nya_http_json_string("int64"));
            nya_object_add(schema, "minimum", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 0 });
            return;

        default:
            nya_object_add(schema, "type", _nya_http_json_string("integer"));
            nya_object_add(schema, "format", _nya_http_json_string("int64"));
            return;
    }
}

NYA_Object* _nya_http_responses_of(NYA_Arena* arena, const NYA_HttpRoute* route) {
    NYA_Object* responses = nya_object_create(arena);

    for (u32 index = 0; index < NYA_HTTP_MAX_STATUSES && route->statuses[index] != NYA_HTTP_STATUS_NONE; index++) {
        NYA_HttpStatus status = route->statuses[index];

        NYA_Object* response = nya_object_create(arena);
        nya_object_add(response, "description", _nya_http_json_string(nya_http_status_text(status)));

        // A success carries the route's response DTO and a refusal carries NYA_HttpProblem — not a convention this file imposes but what nya_http_router_dispatch writes.
        const NYA_TypeReflection* body = status < NYA_HTTP_STATUS_BAD_REQUEST ? route->response_type : nya_reflect_of(NYA_HttpProblem);

        if (body != nullptr && status != NYA_HTTP_STATUS_NO_CONTENT) {
            NYA_Object* reference = nya_object_create(arena);

            NYA_String* path = nya_string_sprintf(arena, "#/components/schemas/%s", body->name);
            nya_object_add(reference, "$ref", _nya_http_json_string(nya_string_to_cstring(arena, path)));

            NYA_Object* media = nya_object_create(arena);
            nya_object_add(media, "schema", _nya_http_json_object(reference));

            NYA_Object* content = nya_object_create(arena);
            nya_object_add(content, "application/json", _nya_http_json_object(media));

            nya_object_add(response, "content", _nya_http_json_object(content));
        }

        NYA_String* code = nya_string_sprintf(arena, "%d", (s32)status);

        nya_object_add(responses, nya_string_to_cstring(arena, code), _nya_http_json_object(response));
    }

    return responses;
}

NYA_Object* _nya_http_operation_of(NYA_Arena* arena, const NYA_HttpRouter* router, const NYA_HttpRoute* route) {
    NYA_Object* operation = nya_object_create(arena);

    nya_object_add(operation, "summary", _nya_http_json_string(route->summary));

    if (route->description != nullptr) nya_object_add(operation, "description", _nya_http_json_string(route->description));

    NYA_ArrayᐸNYA_Valueᐳ* tags = nya_array_create(arena, NYA_Value);
    nya_array_push_back(tags, _nya_http_json_string(router->name));
    nya_object_add(operation, "tags", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *tags });

    // the operation id a generated client turns into a function name: "metrics_get_api_metrics".
    NYA_String* identifier = nya_string_sprintf(arena, "%s_%s_%s", router->name, nya_http_method_text(route->method), route->path);
    nya_string_replace(identifier, "/", "_");
    nya_string_to_lower(identifier);
    nya_object_add(operation, "operationId", _nya_http_json_string(nya_string_to_cstring(arena, identifier)));

    if (route->auth != NYA_HTTP_AUTH_NONE) {
        NYA_ArrayᐸNYA_Valueᐳ* scopes = nya_array_create(arena, NYA_Value);

        const NYA_TypeReflection* scope_type = nya_reflect_of(NYA_HttpScope);

        for (u32 variant = 0; variant < scope_type->variant_count; variant++) {
            if (scope_type->variants[variant].value == 0) continue;
            if (((s64)route->scope & scope_type->variants[variant].value) != scope_type->variants[variant].value) continue;

            nya_array_push_back(scopes, _nya_http_json_string(scope_type->variants[variant].name));
        }

        NYA_Object* requirement = nya_object_create(arena);
        nya_object_add(requirement, "bearer", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *scopes });

        NYA_ArrayᐸNYA_Valueᐳ* security = nya_array_create(arena, NYA_Value);
        nya_array_push_back(security, _nya_http_json_object(requirement));

        nya_object_add(operation, "security", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *security });
    }

    if (route->request_type != nullptr) {
        NYA_Object* reference = nya_object_create(arena);

        NYA_String* path = nya_string_sprintf(arena, "#/components/schemas/%s", route->request_type->name);
        nya_object_add(reference, "$ref", _nya_http_json_string(nya_string_to_cstring(arena, path)));

        NYA_Object* media = nya_object_create(arena);
        nya_object_add(media, "schema", _nya_http_json_object(reference));

        NYA_Object* content = nya_object_create(arena);
        nya_object_add(content, "application/json", _nya_http_json_object(media));

        NYA_Object* body = nya_object_create(arena);
        nya_object_add(body, "required", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
        nya_object_add(body, "content", _nya_http_json_object(content));

        nya_object_add(operation, "requestBody", _nya_http_json_object(body));
    }

    nya_object_add(operation, "responses", _nya_http_json_object(_nya_http_responses_of(arena, route)));

    return operation;
}

void _nya_http_page_escape(NYA_String* html, NYA_ConstCString text) {
    if (text == nullptr) return;

    for (u64 index = 0; text[index] != '\0'; index++) {
        switch (text[index]) {
            case '&': nya_string_extend(html, "&amp;"); break;
            case '<': nya_string_extend(html, "&lt;"); break;
            case '>': nya_string_extend(html, "&gt;"); break;
            case '"': nya_string_extend(html, "&quot;"); break;
            default:  nya_string_push_back(html, (u8)text[index]); break;
        }
    }
}
