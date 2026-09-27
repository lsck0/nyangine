#ifdef NYA_TESTING
#include "nyangine-core/testing/testing_deadline.c"
#include "nyangine-core/testing/testing_property.c"
#include "nyangine-core/testing/testing_simulation.c"
// after the harness they register against; the database half first, since the engine set registers it.
#include "nyangine-core/testing/testing_actions_db.c"
#include "nyangine-core/testing/testing_actions.c"
#include "nyangine-core/testing/testing_traffic.c"
#include "nyangine-core/testing/testing_session.c"
// after the session, whose policy seam it fills.
#include "nyangine-core/testing/testing_agent.c"
#endif
