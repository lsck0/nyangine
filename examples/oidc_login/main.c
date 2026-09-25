/**
 * @file examples/oidc_login/main.c
 *
 * "Log in with X": send a person to a provider, take the code its redirect carries, and come back with
 * a verified identity.
 *
 * ```
 * ./build run example oidc_login                       # says what is missing and stops
 * OIDC_ISSUER=https://accounts.google.com OIDC_CLIENT_ID=... OIDC_REDIRECT_URI=... ./oidc_login.example
 * ```
 *
 * ## Running it with nothing set
 *
 * This is what CI does. A program with no provider configured is not a failure, so it says what is
 * missing and exits zero before anything here would open a socket.
 *
 * ## What the variables are
 *
 * - `OIDC_ISSUER`: the provider's issuer url, no trailing slash — `https://accounts.google.com`, a
 *   Keycloak realm, an Entra tenant.
 * - `OIDC_CLIENT_ID`: the application registered with that provider.
 * - `OIDC_CLIENT_SECRET`: optional. Empty makes this a public client; PKCE protects the exchange either
 *   way, which is the whole point of oidc.h carrying it unconditionally.
 * - `OIDC_REDIRECT_URI`: must equal what was registered with the provider, byte for byte.
 * - `OIDC_SCOPES`: optional, defaults to `"openid email profile"`.
 *
 * ## Why this blocks on a line of stdin
 *
 * There is no redirect to catch here — this is a terminal program, not a web server — so the code the
 * provider's own redirect carries has to come from somewhere a person can paste it. A real server hands
 * `code` to nya_oidc_exchange straight from the callback's query string instead; see http_server.h's
 * `nya_http_request_query_param` and `web_server`'s example for that shape.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define ISSUER_VARIABLE        "OIDC_ISSUER"
#define CLIENT_ID_VARIABLE     "OIDC_CLIENT_ID"
#define CLIENT_SECRET_VARIABLE "OIDC_CLIENT_SECRET"
#define REDIRECT_URI_VARIABLE  "OIDC_REDIRECT_URI"
#define SCOPES_VARIABLE        "OIDC_SCOPES"

#define DEFAULT_SCOPES "openid email profile"

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_unused(argv);

    nya_backtrace_init();
    defer nya_backtrace_deinit();

    NYA_ConstCString issuer        = getenv(ISSUER_VARIABLE);
    NYA_ConstCString client_id     = getenv(CLIENT_ID_VARIABLE);
    NYA_ConstCString client_secret = getenv(CLIENT_SECRET_VARIABLE);
    NYA_ConstCString redirect_uri  = getenv(REDIRECT_URI_VARIABLE);
    NYA_ConstCString scopes        = getenv(SCOPES_VARIABLE);

    if (issuer == nullptr || client_id == nullptr || redirect_uri == nullptr) {
        nya_log_info("This needs three things, and at least one of them is not set:");
        nya_log_info("  " ISSUER_VARIABLE "         the provider's issuer, e.g. https://accounts.google.com");
        nya_log_info("  " CLIENT_ID_VARIABLE "      the application registered with that provider");
        nya_log_info("  " REDIRECT_URI_VARIABLE "   must equal what was registered, byte for byte");
        nya_log_info("  " CLIENT_SECRET_VARIABLE "   optional: empty makes this a public client");
        nya_log_info("  " SCOPES_VARIABLE "          optional: defaults to \"" DEFAULT_SCOPES "\"");
        nya_log_info("Nothing was connected and nothing was sent.");

        return EXIT_SUCCESS;
    }

    NYA_Arena* arena = nya_arena_create(.name = "oidc_login");
    defer      nya_arena_destroy(arena);

    NYA_OidcProvider* provider = nullptr;
    NYA_Error         made     = nya_oidc_create(
        arena,
        (NYA_OidcOptions){
            .issuer        = issuer,
            .client_id     = client_id,
            .client_secret = client_secret,
            .redirect_uri  = redirect_uri,
            .scopes        = scopes != nullptr ? scopes : DEFAULT_SCOPES,
        },
        &provider
    );

    if (!made.ok) {
        nya_log_error("The provider would not start: %s", (NYA_ConstCString)made.message);
        return EXIT_FAILURE;
    }
    defer nya_oidc_destroy(provider);

    NYA_Error discovered = nya_oidc_discover(provider, arena);
    if (!discovered.ok) {
        nya_log_error("Discovery failed: %s", (NYA_ConstCString)discovered.message);
        return EXIT_FAILURE;
    }

    char                   url[NYA_OIDC_MAX_URL * 4] = { 0 };
    NYA_OidcAuthorizeState state                       = { 0 };
    NYA_Error              built                       = nya_oidc_authorize_url(provider, url, sizeof(url), &state);

    if (!built.ok) {
        nya_log_error("Could not build the authorize url: %s", (NYA_ConstCString)built.message);
        return EXIT_FAILURE;
    }

    nya_log_info("Open this in a browser and log in:");
    nya_log_info("%s", url);
    nya_log_info("Then paste the 'code' the redirect carried, and press enter:");

    char code[2048] = { 0 };
    if (fgets(code, sizeof(code), stdin) == nullptr) {
        nya_log_error("No code was read.");
        return EXIT_FAILURE;
    }

    u64 length = strlen(code);
    while (length > 0 && (code[length - 1] == '\n' || code[length - 1] == '\r')) code[--length] = '\0';

    NYA_OidcClaims claims   = { 0 };
    NYA_Error      exchanged = nya_oidc_exchange(provider, arena, code, &state, &claims);

    if (!exchanged.ok) {
        nya_log_error("The exchange was refused: %s", (NYA_ConstCString)exchanged.message);
        return EXIT_FAILURE;
    }

    nya_log_info("Logged in as %s (%s), email %s%s.", claims.subject, claims.name[0] != '\0' ? claims.name : "no name given", claims.email,
                 claims.email_verified ? ", verified" : "");

    if (claims.access_token[0] != '\0') {
        NYA_Object* userinfo = nullptr;
        NYA_Error   asked    = nya_oidc_userinfo(provider, arena, claims.access_token, &userinfo);

        if (asked.ok) {
            NYA_String* serialized = nya_serde_json_serialize(arena, userinfo, NYA_SERDE_NONE);
            nya_log_info("userinfo: %s", nya_string_to_cstring(arena, serialized));
        } else {
            // not fatal: everything this example prints above already came from the id_token itself.
            nya_log_warn("userinfo was not available: %s", (NYA_ConstCString)asked.message);
        }
    }

    return EXIT_SUCCESS;
}
