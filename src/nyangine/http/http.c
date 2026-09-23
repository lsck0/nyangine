// http_types.c first: every other file here folds a header name through _nya_http_lower, which is
// defined there. The rest follow their dependencies, which run in one direction only.
#include "nyangine/http/http_types.c"
/**/
#include "nyangine/http/http_auth.c"
#include "nyangine/http/http_cookie.c"
// after cookie, whose value bound it fits inside, and over crypto's AEAD.
#include "nyangine/http/http_seal.c"
// after seal, which it seals through with whichever key is newest.
#include "nyangine/http/http_keyring.c"
#include "nyangine/http/http_message.c"
// beside http_auth.c and independent of it: it answers the second factor, where that answers the first.
#include "nyangine/http/http_totp.c"
/**/
#include "nyangine/http/http_router.c"
// after the router, whose exchange it reads and whose chain it wraps, and after the message layer,
// whose bodies it decodes: it is the one layer the engine ships.
#include "nyangine/http/http_log.c"
// after the router, whose chain it wraps, and the message layer, whose response it captures and
// replays: another layer a program installs, this one making a retried unsafe request run once.
#include "nyangine/http/http_idempotency.c"
/**/
// after the router, whose route table it builds, and beside the server rather than inside it: the
// bundle is a resource a program merges, not something the listener knows about.
#include "nyangine/http/http_static.c"
/**/
// the wire format, which depends on nothing here, and then the upgrade, which reads the router's
// cross-site check and is what http_server.c hands a socket to.
#include "nyangine/http/http_webhook.c"
#include "nyangine/http/http_websocket.c"
#include "nyangine/http/http_websocket_server.c"
/**/
// after the router, which it dispatches through, and before openapi, which reads its mount table.
#include "nyangine/http/http_server.c"
/**/
#include "nyangine/http/http_openapi.c"
