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
 * Unset is the ordinary case: the read routes work, and PUT /api/metrics/accounting answers 503
 * because this program cannot verify a token it never had a key for.
 * */
#define GNY_WEB_SECRET_ENVIRONMENT "GNYAME_WEB_SECRET"

#define GNY_WEB_GUILD_PATH "/api/guild"

/** Worker threads behind the listener. See where it is passed for why two. */
#define GNY_WEB_WORKERS 2

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE GUILD, OVER HTTP
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The same table the pause menu reads, asked the same questions from a socket. Nothing here knows
 * about roles or ranks beyond what `permission` answers: the route resolves, the resolver decides, and
 * a refusal is a 403 for the same reason it greys a button out in the game.
 */

/**
 * One array index as a key, allocated where the document is.
 *
 * NYA_Object is a dictionary that keeps the pointer it is handed, so a key has to outlive the object:
 * a `char[8]` inside the loop that fills it does not, and the answer is serialized long after.
 * */
NYA_INTERNAL NYA_CString gny_web_index(NYA_Arena* arena, u32 index) {
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "%u", index));
}

/** The roles, what each allows by name, and who holds what. What a role editor draws itself from. */
NYA_INTERNAL NYA_HttpStatus gny_web_guild_read(NYA_HttpExchange* exchange) {
    NYA_Permissions* guild = gny_guild();
    if (guild == nullptr) return NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;

    NYA_Object* body  = nya_object_create(exchange->arena);
    NYA_Object* roles = nya_object_create(exchange->arena);

    for (u32 role = 0; role < nya_permission_role_count(guild); role++) {
        NYA_Object* entry = nya_object_create(exchange->arena);

        nya_object_set(entry, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)nya_permission_role_name(guild, role) });
        nya_object_set(entry, "position", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = nya_permission_role_position(guild, role) });

        // by name rather than as a number: an editor that had to know the bits would be an editor
        // written for this game, and the whole point is that it is not.
        NYA_Object*    allowed = nya_object_create(exchange->arena);
        NYA_Permission allows  = nya_permission_role_allows(guild, role);
        u32            count   = 0;

        for (u32 bit = 0; bit < 64; bit++) {
            NYA_Permission one = 1ULL << bit;

            if ((allows & one) == 0 || nya_permission_label(guild, one)[0] == '\0') continue;

            // out of the exchange's arena and not off the stack: a document keeps the pointer it was
            // given as the key, and a buffer inside this loop is gone before anything serializes it.
            NYA_CString index = gny_web_index(exchange->arena, count++);

            nya_object_set(allowed, index, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)nya_permission_label(guild, one) });
        }

        nya_object_set(entry, "allows", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *allowed });

        NYA_CString index = gny_web_index(exchange->arena, role);

        nya_object_set(roles, index, (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry });
    }

    nya_object_set(body, "roles", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *roles });

    // every permission that has a name, which is the vocabulary a role editor draws its rows from: it
    // asks the table what exists rather than being compiled against this game's bits.
    NYA_Object*    vocabulary = nya_object_create(exchange->arena);
    NYA_Permission labelled   = nya_permission_labelled(guild);
    u32            named      = 0;

    for (u32 bit = 0; bit < 64; bit++) {
        if ((labelled & (1ULL << bit)) == 0) continue;

        NYA_CString index = gny_web_index(exchange->arena, named++);

        nya_object_set(vocabulary, index,
                       (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)nya_permission_label(guild, 1ULL << bit) });
    }

    nya_object_set(body, "permissions", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *vocabulary });
    nya_object_set(body, "members", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = nya_permission_subject_count(guild) });
    nya_object_set(body, "owner", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_permissions_owner(guild) });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Drops a player, as the subject the token names.
 *
 * By the time this runs the caller has been resolved against the guild table and found to hold KICK
 * here: the route declares it, so the extractor checked before this was called and a handler cannot be
 * reached unchecked. What is left is the hierarchy, which is about these two players rather than about
 * the route, and gny_guild_kick is where that lives.
 * */
NYA_INTERNAL NYA_HttpStatus gny_web_guild_kick(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity) {
    if (gny_guild() == nullptr) return NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;

    u64 actor  = gny_guild_subject_of(identity);
    u64 target = 0;

    char wanted[24] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "peer", wanted, sizeof(wanted)) ||
        !nya_type_parse(NYA_TYPE_U64, (const u8*)wanted, strlen(wanted), &target)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "which peer: ?peer=<subject>");
    }

    NYA_Error kicked = gny_guild_kick(actor, target);

    if (!kicked.ok) {
        // the same refusal the button in the pause menu is greyed out by, arriving over a socket.
        if (kicked.kind == NYA_ERROR_PERMISSION_DENIED) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, (NYA_ConstCString)kicked.message);
        }

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, (NYA_ConstCString)kicked.message);
    }

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/*
 * Both routes are NYA_HTTP_AFFINITY_MAIN, because both read the guild table the game itself is
 * writing: a kick from the pause menu and a kick over a socket are the same call into the same table,
 * and that table has no lock. Answering them inside the frame is what makes "one question, two
 * callers" true rather than nearly true; see http_server.h for what the other affinity promises.
 */
NYA_INTERNAL const NYA_HttpRoute GNY_WEB_GUILD_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_QUERY,
     .path        = GNY_WEB_GUILD_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = gny_web_guild_read,
     .summary     = "The session's roles and who is in it",
     .description = "The same table the game's pause menu reads. Permissions come back by name, so an editor needs to know nothing "
                        "about this game's bits.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method             = NYA_HTTP_METHOD_DELETE,
     .path               = GNY_WEB_GUILD_PATH,
     .auth               = NYA_HTTP_AUTH_BEARER,
     .affinity           = NYA_HTTP_AFFINITY_MAIN,
     .permission         = GNY_PERMISSION_KICK,
     .resource           = GNY_GUILD_SESSION,
     .handler_identified = gny_web_guild_kick,
     .summary            = "Drops a player",
     .description        = "`?peer=<subject>`. The route declares the permission, so the caller is resolved against the guild table "
                           "before the handler runs; what is left to the handler is whether they outrank the player they named.",
     .statuses           = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                             NYA_HTTP_STATUS_SERVICE_UNAVAILABLE },
     },
};

NYA_INTERNAL const NYA_HttpRouter GNY_WEB_GUILD_ROUTER = {
    .name        = "guild",
    .routes      = GNY_WEB_GUILD_ROUTES,
    .route_count = nya_carray_length(GNY_WEB_GUILD_ROUTES),
};

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
        nya_log_warn(
            "%s is set but unusable (%s); the routes that need a token will answer 503.",
            GNY_WEB_SECRET_ENVIRONMENT,
            (NYA_ConstCString)read.message
        );
    }

    /*
     * One root layer, outermost, so the record it writes covers the whole exchange and reports whatever
     * anything inside decided. How much it writes is `engine.http_log` in assets/config/engine.nya,
     * already applied by the config watch and applied again whenever that file is saved: nothing here
     * sets a level, because a level set here would be a level that cannot be changed without a rebuild.
     */
    static const NYA_HttpLayerFn LAYERS[] = { nya_http_layer_log };

    /*
     * On its own thread, with two workers. The game is what this program is for, so the frame pays for
     * as little of the web interface as it can: the listener accepts and reads, a worker verifies a
     * token and renders an answer, and what is left for the frame is the routes that read the guild
     * table, which is every route this file mounts of its own. Two is enough for an interface one
     * person has open; see NYA_HTTP_MAX_WORKERS.
     */
    NYA_Error started = nya_system_http_init((NYA_HttpConfig){
        .port        = (u16)port,
        .workers     = GNY_WEB_WORKERS,
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

    /*
     * And the session's own table, which is the same one the game reads: one question, two callers. The
     * guild installs it, because the guild is what ends when a session ends; a route that demands a
     * permission with no session answers 503 rather than reading a table that has been freed.
     */
    NYA_EXPECT(nya_http_server_merge(&GNY_WEB_GUILD_ROUTER), "while mounting the guild resource");

    /*
     * The metrics line spells the verb out: the reads are QUERY, so a browser pointed at that path
     * gets a 405 and the first thing anybody would try is the wrong thing. The page is a GET and is
     * the link that works in a browser, which is why it comes second and unqualified.
     */
    nya_log_info(
        "Metrics: curl -X QUERY http://127.0.0.1:%llu%s. The routes, as a page: http://127.0.0.1:%llu%s",
        (unsigned long long)port,
        NYA_HTTP_METRICS_PATH,
        (unsigned long long)port,
        NYA_HTTP_DOCS_PATH
    );
}

void gny_web_stop(void) {
    nya_system_http_deinit();
}
