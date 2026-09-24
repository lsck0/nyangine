// accounts_throttle.c first: it names nothing else here and the login half calls it.
#include "nyangine/accounts/accounts_throttle.c"
/**/
// accounts_user.c first: it owns the module's static state and the two tables the session half uses.
#include "nyangine/accounts/accounts_user.c"
/**/
#include "nyangine/accounts/accounts_session.c"
// last: it names the users table, the sessions table and both of their halves.
#include "nyangine/accounts/accounts_identity.c"
// last: it names the recovery table on the state the user half owns, and the throttle.
#include "nyangine/accounts/accounts_recovery.c"
// after the user half, whose tables it names, and over crypto's Ed25519 verify and serde's CBOR reader.
#include "nyangine/accounts/accounts_passkey.c"
#include "nyangine/accounts/accounts_invite.c"
#include "nyangine/accounts/accounts_audit.c"
