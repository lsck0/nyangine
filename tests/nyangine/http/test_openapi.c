/**
 * The generated schema.
 *
 * The property being checked is that the document comes from the code: every route's path, method,
 * status list and DTO has to appear in it without anything here having written the document down, and
 * the schema for a DTO has to describe what the serializer actually writes rather than the C layout.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Starts the server on a port the system chose; see test_server.c's copy for why not a fixed window. */
static u16 start_server(void) {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port }), "while starting the test server");

    return port;
}

/** The value at `key`, as a string, or null. */
static NYA_ConstCString string_at(const NYA_Object* object, NYA_ConstCString key) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);

    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

/** The object at `key`, or null. */
static NYA_Object* object_at(const NYA_Object* object, NYA_ConstCString key) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);

    if (value == nullptr || value->type != NYA_TYPE_OBJECT) return nullptr;

    return &value->as_object;
}

/** Whether the array at `key` holds the string `wanted`. */
static b8 array_contains(const NYA_Object* object, NYA_ConstCString key, NYA_ConstCString wanted) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);

    if (value == nullptr || value->type != NYA_TYPE_ARRAY) return false;

    for (u64 index = 0; index < value->as_array.length; index++) {
        NYA_Value* item = &value->as_array.items[index];

        if (item->type == NYA_TYPE_STRING && nya_string_equals(item->as_string, wanted)) return true;
    }

    return false;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_openapi");
    defer      nya_arena_destroy(arena);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a DTO's schema describes what the serializer writes, not the C layout.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Object* schema = nya_http_openapi_schema(arena, nya_reflect_of(NYA_HttpCeilingDto));
        nya_assert(schema != nullptr);

        nya_assert(nya_string_equals(string_at(schema, "type"), "object"));

        NYA_Object* properties = object_at(schema, "properties");
        nya_assert(properties != nullptr);

        // `char name[64]` goes out as text, so it is described as text.
        NYA_Object* name = object_at(properties, "name");
        nya_assert(name != nullptr);
        nya_assert(nya_string_equals(string_at(name, "type"), "string"), "a char array is a string on the wire, not a list of numbers");

        NYA_Object* capacity = object_at(properties, "capacity");
        nya_assert(capacity != nullptr);
        nya_assert(nya_string_equals(string_at(capacity, "type"), "integer"));
        nya_assert(nya_object_get(capacity, "minimum") != nullptr, "an unsigned field cannot be negative and the schema says so");

        NYA_Object* fullness = object_at(properties, "fullness");
        nya_assert(fullness != nullptr);
        nya_assert(nya_string_equals(string_at(fullness, "type"), "number"));

        // every field of a DTO is written, so every field is required.
        nya_assert(array_contains(schema, "required", "name"));
        nya_assert(array_contains(schema, "required", "capacity"));
        nya_assert(array_contains(schema, "required", "fullness"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a fixed array of structs, and a boolean.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Object* schema = nya_http_openapi_schema(arena, nya_reflect_of(NYA_HttpCeilingsDto));
        nya_assert(schema != nullptr);

        NYA_Object* properties = object_at(schema, "properties");
        nya_assert(properties != nullptr);

        NYA_Object* rows = object_at(properties, "rows");
        nya_assert(rows != nullptr);
        nya_assert(nya_string_equals(string_at(rows, "type"), "array"));

        NYA_Object* items = object_at(rows, "items");
        nya_assert(items != nullptr);
        nya_assert(nya_string_equals(string_at(items, "type"), "object"), "the element type is described, not left opaque");

        // a fixed C array has one length, and the schema carries it both ways.
        nya_assert(nya_object_get(rows, "minItems") != nullptr);
        nya_assert(nya_object_get(rows, "maxItems") != nullptr);

        NYA_Object* boolean = object_at(object_at(nya_http_openapi_schema(arena, nya_reflect_of(NYA_HttpAccountingDto)), "properties"), "enabled");
        nya_assert(boolean != nullptr);
        nya_assert(nya_string_equals(string_at(boolean, "type"), "boolean"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: an integer of flags is a list of names, because that is what is written.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Object* schema = nya_http_openapi_schema(arena, nya_reflect_of(NYA_HttpIdentity));
        nya_assert(schema != nullptr);

        NYA_Object* scope = object_at(object_at(schema, "properties"), "scope");
        nya_assert(scope != nullptr);

        nya_assert(nya_string_equals(string_at(scope, "type"), "array"), "a set of flags serializes as a list of names");

        NYA_Object* items = object_at(scope, "items");
        nya_assert(items != nullptr);
        nya_assert(nya_string_equals(string_at(items, "type"), "string"));
        nya_assert(array_contains(items, "enum", "NYA_HTTP_SCOPE_READ"));
        nya_assert(array_contains(items, "enum", "NYA_HTTP_SCOPE_WRITE"));
        nya_assert(!array_contains(items, "enum", "NYA_HTTP_SCOPE_NONE"), "a zero flag never appears in a list of set flags");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the document describes what is mounted, and nothing else.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // nothing mounted, nothing to describe.
        NYA_String* early = nullptr;
        nya_assert(!nya_http_openapi_document(arena, &early).ok);

        u16   port = start_server();
        defer nya_system_http_deinit();

        nya_unused(port);

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);
        nya_assert(nya_http_server_merge(nya_http_openapi_router()).ok);

        NYA_String* json = nullptr;
        nya_assert(nya_http_openapi_document(arena, &json).ok);
        nya_assert(json != nullptr && json->length > 0);

        NYA_Object* document = nullptr;
        nya_assert(nya_deserialize(arena, (const u8*)json->items, json->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &document).ok);
        nya_assert(document != nullptr);

        /*
         * 3.2.0 and not 3.1.0, because `query` became a field of the Path Item Object in 3.2 and a
         * QUERY route has nowhere legal to sit in a 3.1 document. See the note in http_openapi.h.
         */
        nya_assert(nya_string_equals(string_at(document, "openapi"), "3.2.0"));

        NYA_Object* paths = object_at(document, "paths");
        nya_assert(paths != nullptr);

        // every mounted path is in it, and each under the methods its routes answer.
        NYA_Object* metrics = object_at(paths, NYA_HTTP_METRICS_PATH);
        nya_assert(metrics != nullptr);
        nya_assert(object_at(metrics, "query") != nullptr, "a read is a QUERY, and the document says so in the field the spec gives it");
        nya_assert(object_at(metrics, "get") == nullptr, "a method with no route does not appear");
        nya_assert(object_at(metrics, "post") == nullptr);

        // the two routes that stay GET, because a browser and a generator have no other verb.
        NYA_Object* schema_path = object_at(paths, NYA_HTTP_OPENAPI_PATH);
        nya_assert(schema_path != nullptr && object_at(schema_path, "get") != nullptr);

        NYA_Object* docs_path = object_at(paths, NYA_HTTP_DOCS_PATH);
        nya_assert(docs_path != nullptr && object_at(docs_path, "get") != nullptr);

        NYA_Object* accounting = object_at(paths, NYA_HTTP_METRICS_ACCOUNTING_PATH);
        nya_assert(accounting != nullptr);

        NYA_Object* operation = object_at(accounting, "put");
        nya_assert(operation != nullptr, "setting a flag to a value is a PUT: it creates nothing and repeats the same");

        // the summary is the route's, so a route that changes its summary changes the document.
        nya_assert(string_at(operation, "summary") != nullptr);
        nya_assert(string_at(operation, "operationId") != nullptr);
        nya_assert(array_contains(operation, "tags", "metrics"), "a route is tagged with the router it came from");

        // a route behind the extractor carries a security requirement naming its scope.
        NYA_Value* security = nya_object_get(operation, "security");
        nya_assert(security != nullptr && security->type == NYA_TYPE_ARRAY && security->as_array.length == 1);
        nya_assert(array_contains(&security->as_array.items[0].as_object, "bearer", "NYA_HTTP_SCOPE_WRITE"));

        // every status the route declared is a response, with the error ones carrying the problem body.
        NYA_Object* responses = object_at(operation, "responses");
        nya_assert(responses != nullptr);
        nya_assert(object_at(responses, "200") != nullptr);
        nya_assert(object_at(responses, "401") != nullptr);
        nya_assert(object_at(responses, "403") != nullptr);
        nya_assert(object_at(responses, "400") != nullptr);

        NYA_Object* refusal = object_at(object_at(object_at(object_at(responses, "401"), "content"), "application/json"), "schema");
        nya_assert(refusal != nullptr);
        nya_assert(nya_string_contains(string_at(refusal, "$ref"), "NYA_HttpProblem"));

        // and the DTOs the routes name are in components, under their own C type names.
        NYA_Object* schemas = object_at(object_at(document, "components"), "schemas");
        nya_assert(schemas != nullptr);
        nya_assert(object_at(schemas, "NYA_HttpMetricsDto") != nullptr);
        nya_assert(object_at(schemas, "NYA_HttpAccountingDto") != nullptr);
        nya_assert(object_at(schemas, "NYA_HttpProblem") != nullptr);

        nya_assert(object_at(object_at(object_at(document, "components"), "securitySchemes"), "bearer") != nullptr);

        // unmounting a resource takes it out of the document, which is the point of generating it.
        nya_http_server_unmerge(nya_http_metrics_router());

        NYA_String* smaller = nullptr;
        nya_assert(nya_http_openapi_document(arena, &smaller).ok);
        nya_assert(!nya_string_contains(smaller, NYA_HTTP_METRICS_PATH), "the document describes what is mounted now, not what once was");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the page is generated from the same walk and escapes what it prints.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16   port = start_server();
        defer nya_system_http_deinit();

        nya_unused(port);

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);

        NYA_String* html = nullptr;
        nya_assert(nya_http_openapi_page(arena, &html).ok);

        nya_assert(nya_string_contains(html, NYA_HTTP_METRICS_PATH));
        nya_assert(nya_string_contains(html, "metrics"));
        nya_assert(nya_string_contains(html, "QUERY "), "the page names the verb a reader has to send");
        nya_assert(nya_string_contains(html, NYA_HTTP_OPENAPI_PATH), "the page points at the machine readable version");

        // a summary containing markup would otherwise become markup.
        nya_assert(!nya_string_contains(html, "<script"));
    }

    printf("PASSED: http openapi\n");

    return EXIT_SUCCESS;
}
