// http_types.c first: every other file here folds a header name through _nya_http_lower, which is
// defined there. The rest follow their dependencies, which run in one direction only.
#include "nyangine/http/http_types.c"
/**/
#include "nyangine/http/http_auth.c"
#include "nyangine/http/http_message.c"
/**/
#include "nyangine/http/http_router.c"
/**/
// after the router, which it dispatches through, and before openapi, which reads its mount table.
#include "nyangine/http/http_server.c"
/**/
#include "nyangine/http/http_metrics.c"
#include "nyangine/http/http_openapi.c"
