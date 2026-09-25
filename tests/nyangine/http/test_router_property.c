/**
 * The router's match as laws rather than examples: a path is matched whole and exactly, so a method and
 * path that were registered resolve to that very route and nothing else; a path that was never
 * registered resolves to nothing and is reported as not existing, which is the difference between a 404
 * and a 405; a path that exists under other methods resolves to nothing for a method it does not carry,
 * but is reported as existing; and a HEAD with no HEAD route of its own falls back to the path's GET, or
 * failing that its QUERY, and to nothing when the path is neither.
 *
 * The example cases — the layer order, the extractor, a short-circuiting layer — are driven through
 * nya_http_router_dispatch in test_router.c. This is the part a handful of tables cannot reach: the
 * match stated over route tables nobody hand-picked, so a fallback that fires one route early or a
 * prefix mistaken for the whole path shows up as a table the law disagrees with.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Cases per law. Thousands of random little route tables, each matched against paths in and out of it. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. "routeprp" in ASCII. */
#define SEED 0x726F757465707270ULL

/** Routes in a generated table at most. A handful is enough for a path to carry several verbs and collide. */
#define ROUTES_MAX 12

/** The distinct paths a table draws from. Few, so a path is shared across verbs and re-drawn as a lookup. */
#define PATHS_MAX 6

/* HELPERS */

/** The absolute paths a table and its lookups are drawn from; a couple share a prefix, to catch a partial match. */
static NYA_ConstCString PATHS[PATHS_MAX] = { "/a", "/b", "/api", "/api/v2", "/api/v2/items", "/health" };

/** The methods a route carries, spanning the two safe read verbs a HEAD falls back to and the writers it does not. */
static const NYA_HttpMethod METHODS[] = {
    NYA_HTTP_METHOD_GET, NYA_HTTP_METHOD_QUERY, NYA_HTTP_METHOD_POST, NYA_HTTP_METHOD_PUT, NYA_HTTP_METHOD_DELETE,
};
#define METHODS_COUNT nya_carray_length(METHODS)

/** One generated table: routes with only the method and path a lookup reads, and a router pointing at them. */
typedef struct {
    NYA_HttpRoute  routes[ROUTES_MAX];
    u32            route_count;
    NYA_HttpRouter router;
} Table;

/**
 * Draws a table of unique (method, path) routes. Uniqueness is what gives the law a single right answer:
 * a table with one verb per path per shape is the one the server checks for, so an exact lookup has at
 * most one route it can mean.
 * */
static void draw_table(NYA_Property* property, OUT Table* table) {
    table->route_count = (u32)nya_property_draw_below(property, ROUTES_MAX + 1);

    u32 kept = 0;
    for (u32 index = 0; index < table->route_count; index++) {
        NYA_HttpMethod   method = METHODS[nya_property_draw_below(property, METHODS_COUNT)];
        NYA_ConstCString path   = PATHS[nya_property_draw_below(property, PATHS_MAX)];

        // Drop a pair already in the table, so the same method and path are never registered twice.
        b8 duplicate = false;
        for (u32 seen = 0; seen < kept; seen++) {
            if (table->routes[seen].method == method && nya_string_equals(table->routes[seen].path, path)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        table->routes[kept++] = (NYA_HttpRoute){ .method = method, .path = path };
    }
    table->route_count = kept;

    table->router = (NYA_HttpRouter){ .name = "generated", .routes = table->routes, .route_count = table->route_count };
}

/** Whether the table registers `method` exactly on `path`, and the route it would be if so. */
static const NYA_HttpRoute* exact(const Table* table, NYA_HttpMethod method, NYA_ConstCString path) {
    for (u32 index = 0; index < table->route_count; index++) {
        if (table->routes[index].method == method && nya_string_equals(table->routes[index].path, path)) return &table->routes[index];
    }
    return nullptr;
}

/** Whether any method at all is registered on `path`: the model's answer for out_path_exists. */
static b8 path_present(const Table* table, NYA_ConstCString path) {
    for (u32 index = 0; index < table->route_count; index++) {
        if (nya_string_equals(table->routes[index].path, path)) return true;
    }
    return false;
}

/* LAWS */

/**
 * A non-HEAD lookup resolves to the route registered on exactly that method and path, and to nothing
 * when none is; and out_path_exists is set to whether any method is registered on the path, whatever the
 * lookup's own method. That last is the whole of the 404-versus-405 decision the dispatcher makes.
 * */
static b8 law_exact_match(NYA_Property* property) {
    Table table = { 0 };
    draw_table(property, &table);

    NYA_HttpMethod   method = METHODS[nya_property_draw_below(property, METHODS_COUNT)]; // never HEAD: the fallback is its own law.
    NYA_ConstCString path   = PATHS[nya_property_draw_below(property, PATHS_MAX)];

    const NYA_HttpRouter* routers[]     = { &table.router };
    b8                    path_exists   = false;
    const NYA_HttpRoute*  found         = nya_http_router_find(routers, 1, method, path, &path_exists);
    const NYA_HttpRoute*  expected      = exact(&table, method, path);

    if (path_exists != path_present(&table, path)) {
        nya_property_note(property, "path '%s' existence reported as %d, model says %d", path, path_exists, path_present(&table, path));
        return false;
    }

    if (expected == nullptr) {
        nya_property_note(property, "'%s' with method %d matched a route the model has none for", path, method);
        return found == nullptr;
    }

    // The pointer must be that route: matching a different verb or a prefix would be a wrong answer.
    nya_property_note(property, "'%s' with method %d did not resolve to its registered route", path, method);
    return found == expected;
}

/**
 * A HEAD with no HEAD route of its own falls back to the path's GET, or failing that its QUERY, and to
 * nothing when the path carries neither. The fallback never crosses to a path the HEAD did not name, and
 * never answers from a writer.
 * */
static b8 law_head_falls_back(NYA_Property* property) {
    Table table = { 0 };
    draw_table(property, &table);

    NYA_ConstCString path = PATHS[nya_property_draw_below(property, PATHS_MAX)];

    const NYA_HttpRouter* routers[]   = { &table.router };
    b8                    path_exists = false;
    const NYA_HttpRoute*  found       = nya_http_router_find(routers, 1, NYA_HTTP_METHOD_HEAD, path, &path_exists);

    // The model of the fallback: a HEAD route wins outright, then GET, then QUERY, then nothing.
    const NYA_HttpRoute* expected = exact(&table, NYA_HTTP_METHOD_HEAD, path);
    if (expected == nullptr) expected = exact(&table, NYA_HTTP_METHOD_GET, path);
    if (expected == nullptr) expected = exact(&table, NYA_HTTP_METHOD_QUERY, path);

    if (path_exists != path_present(&table, path)) {
        nya_property_note(property, "HEAD on '%s' reported existence %d, model says %d", path, path_exists, path_present(&table, path));
        return false;
    }

    nya_property_note(property, "HEAD on '%s' did not fall back to the route the model expects", path);
    return found == expected;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("a non-HEAD lookup matches its exact route", CASES, SEED, law_exact_match);
    failures += nya_property_check("a HEAD falls back to GET then QUERY", CASES, SEED, law_head_falls_back);

    return failures == 0 ? 0 : 1;
}
