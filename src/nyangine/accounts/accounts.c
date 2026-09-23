// accounts_throttle.c first: it names nothing else here and the login half calls it.
#include "nyangine/accounts/accounts_throttle.c"
/**/
// accounts_user.c first: it defines the module's one static state, which the session half reads, and
// it owns the two tables the session half stores its rows in.
#include "nyangine/accounts/accounts_user.c"
/**/
#include "nyangine/accounts/accounts_session.c"
