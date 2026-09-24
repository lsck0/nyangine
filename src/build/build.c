// The preprocessor passes, in the order they depend on each other: stale.c first, since every other
// pass opens by calling it.
#include "build/pp/stale.c"
#include "build/pp/asset.c"
#include "build/pp/cheatsheet.c"
#include "build/pp/i18n.c"
#include "build/pp/lambda.c"
#include "build/pp/luabind.c"
#include "build/pp/reflection.c"
#include "build/pp/watch.c"
/**/
#include "build/hooks.c"
#include "build/test.c"
// After test.c: they borrow NYA_BuildRulePointer and its derived array, which test.c declares.
#include "build/bench.c"
#include "build/lint.c"
// After lint.c, whose lint_run it calls before clang-tidy.
#include "build/check.c"
// A sibling gate: spell-checks the prose and code. Independent of the above; here beside check.c
// because it is the other read-only quality gate.
#include "build/typos.c"
// Another sibling gate: bounded model checking of the untrusted-input parsers with CBMC. Beside the
// other read-only gates, and independent of them.
#include "build/verify.c"
// Another sibling gate, over the commit messages rather than the code: a thin bridge onto the shared
// shell linter the commit-msg hook also runs.
#include "build/commit.c"
// Another sibling gate beside check.c and typos.c: clang-format over the hand-written C. Advisory, and
// not on the critical path, so it sits with the other read-only quality gates.
#include "build/format.c"
// Before dist.c: it defines build_capture, and writes the CHANGELOG.md every distribution ships.
#include "build/changelog.c"
// After changelog.c: it calls build_capture, which changelog.c defines.
#include "build/sbom.c"
#include "build/dist.c"
#include "build/example.c"
#include "build/plugin.c"
// After test.c: both name the host flags and the sanitizer environment a test binary runs under.
#include "build/fuzz.c"
#include "build/simulation.c"
#include "build/agent.c"
// Last: the commands it defines name rules and handlers from all of the above.
#include "build/cli.c"
