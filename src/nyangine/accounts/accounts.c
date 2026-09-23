// accounts_throttle.c first: it names nothing else here and the login half calls it.
#include "nyangine/accounts/accounts_throttle.c"
/**/
// accounts_user.c first: it defines the module's one static state, which the session half reads, and
// it owns the two tables the session half stores its rows in.
#include "nyangine/accounts/accounts_user.c"
/**/
#include "nyangine/accounts/accounts_session.c"
// last: it names the users table, the sessions table and both of their halves.
#include "nyangine/accounts/accounts_identity.c"
// last: it names the recovery table on the state the user half owns, and the throttle.
#include "nyangine/accounts/accounts_recovery.c"
#include "nyangine/accounts/accounts_invite.c"
