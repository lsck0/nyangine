// http_types.c first: every file folds a header name through _nya_http_lower, defined there; the rest follow dependencies, one direction only.
#include "nyangine-core/http/http_types.c"
/**/
#include "nyangine-core/http/http_auth.c"
#include "nyangine-core/http/http_cookie.c"
// after cookie, whose value bound it fits inside, and over crypto's AEAD.
#include "nyangine-core/http/http_seal.c"
// after seal, which it seals through with whichever key is newest.
#include "nyangine-core/http/http_keyring.c"
// over crypto's Ed25519/SHA-256 and independent of the server: a signed statement an origin serves and a mirror checks — data a program builds, not a socket.
#include "nyangine-core/http/http_attestation.c"
#include "nyangine-core/http/http_message.c"
// beside http_auth.c and independent of it: it answers the second factor, where that answers the first.
#include "nyangine-core/http/http_totp.c"
/**/
#include "nyangine-core/http/http_router.c"
// after the router, whose table it reads, and over serde/the message layer whose encoding it mirrors: it calls a route by its table entry, request DTO in, response DTO out.
#include "nyangine-core/http/http_client.c"
// after the router, whose route table it builds on: two routes an orchestrator polls for liveness and readiness, the readiness checks a small registry the program composes.
#include "nyangine-core/http/http_health.c"
// after the router, whose exchange it reads and whose chain it wraps, and the message layer whose bodies it decodes: the one layer the engine ships.
#include "nyangine-core/http/http_log.c"
// after the router, whose chain it wraps, and the message layer whose response it captures and replays: a layer a program installs to make a retried unsafe request run once.
#include "nyangine-core/http/http_idempotency.c"
// after the router, whose chain it wraps, and seal/crypto, which it mints and opens a sealed challenge through: the abuse layer a public server installs where there's no IP to rate-limit.
#include "nyangine-core/http/http_pow.c"
/**/
// after the router, whose route table it builds, and beside the server not inside it: the bundle is a resource a program merges, not something the listener knows.
#include "nyangine-core/http/http_static.c"
/**/
// the discoverability surface: a bounded document builder and serve registry, then the four documents built on them; after the message layer and router. A program merges nya_http_doc_router() like the bundle above.
#include "nyangine-core/http/http_doc.c"
#include "nyangine-core/http/http_sitemap.c"
#include "nyangine-core/http/http_feed.c"
#include "nyangine-core/http/http_robots.c"
#include "nyangine-core/http/http_llms.c"
/**/
// the wire format, which depends on nothing here, then the upgrade, which reads the router's cross-site check and is what http_server.c hands a socket to.
#include "nyangine-core/http/http_webhook.c"
#include "nyangine-core/http/http_websocket.c"
#include "nyangine-core/http/http_websocket_server.c"
// after the websocket server, whose framing and connection table it wraps as a net transport, and over net (compiled before http): a browser peer of a native server.
#include "nyangine-core/http/http_net_websocket.c"
/**/
// development-only live reload: a push stream on the websocket server above and the change watch over the static bundle's fingerprint, so it follows both. Compiled out of a shipping build.
#include "nyangine-core/http/http_livereload.c"
/**/
// after the router, which it dispatches through, and before openapi, which reads its mount table.
#include "nyangine-core/http/http_server.c"
/**/
#include "nyangine-core/http/http_openapi.c"
