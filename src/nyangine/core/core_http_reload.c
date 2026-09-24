#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RESOLVERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A resolver is a token in, a function pointer out. nya_callback_get answers with a void*, so each of
 * these casts it to the route's function-pointer type — the same explicit cast core_system.c uses on
 * its phase functions, which is what keeps the cast-through-void lint quiet.
 */

NYA_INTERNAL NYA_HttpHandlerFn _nya_http_reload_handler(u64 token) {
    return (NYA_HttpHandlerFn)nya_callback_get(token);
}

NYA_INTERNAL NYA_HttpIdentifiedFn _nya_http_reload_identified(u64 token) {
    return (NYA_HttpIdentifiedFn)nya_callback_get(token);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_http_router_reloadable(void) {
    nya_http_router_resolvers_set(_nya_http_reload_handler, _nya_http_reload_identified);
}
