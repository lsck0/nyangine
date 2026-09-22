// The preprocessor passes, in the order they depend on each other: stale.c first, since every other
// pass opens by calling it.
#include "build/pp/stale.c"
#include "build/pp/asset.c"
#include "build/pp/cheatsheet.c"
#include "build/pp/i18n.c"
#include "build/pp/luabind.c"
#include "build/pp/reflection.c"
/**/
#include "build/hooks.c"
#include "build/test.c"
// After test.c: they borrow NYA_BuildRulePointer and its derived array, which test.c declares.
#include "build/bench.c"
#include "build/lint.c"
// After lint.c, whose lint_run it calls before clang-tidy.
#include "build/check.c"
// Before dist.c: it defines build_capture, and writes the CHANGELOG.md every distribution ships.
#include "build/changelog.c"
#include "build/dist.c"
#include "build/example.c"
// After test.c: both name the host flags and the sanitizer environment a test binary runs under.
#include "build/fuzz.c"
#include "build/simulation.c"
#include "build/agent.c"
// Last: the commands it defines name rules and handlers from all of the above.
#include "build/cli.c"
