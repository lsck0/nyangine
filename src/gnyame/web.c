#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The port, as a decimal string. Unset means the whole feature stays off and costs nothing. */
#define GNY_WEB_PORT_ENVIRONMENT "GNYAME_WEB_PORT"

/**
 * Where the token signing secret comes from, when anybody wants the write routes.
 *
 * Unset is the ordinary case: the read routes work, and POST /api/metrics/accounting answers 503
 * because this program cannot verify a token it never had a key for.
 * */
#define GNY_WEB_SECRET_ENVIRONMENT "GNYAME_WEB_SECRET"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_web_start(void) {
    NYA_ConstCString requested = getenv(GNY_WEB_PORT_ENVIRONMENT);

    // off unless asked for. Nothing above this line allocated, bound or registered anything.
    if (requested == nullptr || requested[0] == '\0') return;

    u64 port = 0;

    if (!nya_type_parse(NYA_TYPE_U64, (const u8*)requested, strlen(requested), &port) || port == 0 || port > U16_MAX) {
        nya_log_error("%s is '%s', which is not a port; the web interface will not start.", GNY_WEB_PORT_ENVIRONMENT, requested);
        return;
    }

    u8  secret[NYA_HTTP_MAX_SECRET_BYTES] = { 0 };
    u64 secret_size                       = 0;

    NYA_Error read = nya_http_secret_from_environment(GNY_WEB_SECRET_ENVIRONMENT, secret, sizeof(secret), &secret_size);

    if (!read.ok && read.kind != NYA_ERROR_NOT_FOUND) {
        nya_log_warn("%s is set but unusable (%s); the routes that need a token will answer 503.", GNY_WEB_SECRET_ENVIRONMENT,
                     (NYA_ConstCString)read.message);
    }

    // one root layer, outermost, so the line it writes covers the whole exchange and reports whatever
    // anything inside decided.
    static const NYA_HttpLayerFn LAYERS[] = { nya_http_layer_log };

    NYA_Error started = nya_system_http_init((NYA_HttpConfig){
        .port        = (u16)port,
        .secret      = secret_size > 0 ? secret : nullptr,
        .secret_size = secret_size,
        .layers      = LAYERS,
        .layer_count = nya_carray_length(LAYERS),
    });

    // the secret is in the server's own copy now; this one does not outlive the call.
    nya_memset(secret, 0, sizeof(secret));

    if (!started.ok) {
        nya_log_error("The web interface could not start on port %llu: %s", (unsigned long long)port, (NYA_ConstCString)started.message);
        return;
    }

    /*
     * Two resources: this program's own numbers, and the schema that describes them. Both are static
     * tables, so mounting them is two pointers and no allocation.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()), "while mounting the metrics resource");
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while mounting the schema resource");

    nya_log_info("Metrics at http://127.0.0.1:%llu%s, the routes at http://127.0.0.1:%llu%s.", (unsigned long long)port, NYA_HTTP_METRICS_PATH,
                 (unsigned long long)port, NYA_HTTP_DOCS_PATH);
}

void gny_web_stop(void) {
    nya_system_http_deinit();
}
