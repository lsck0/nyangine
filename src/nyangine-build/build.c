// The preprocessor passes, in the order they depend on each other: stale.c first, since every other
// pass opens by calling it.
#include "nyangine-build/pp/stale.c"
#include "nyangine-build/pp/asset.c"
#include "nyangine-build/pp/cheatsheet.c"
#include "nyangine-build/pp/i18n.c"
#include "nyangine-build/pp/lambda.c"
#include "nyangine-build/pp/luabind.c"
#include "nyangine-build/pp/reflection.c"
#include "nyangine-build/pp/watch.c"
/**/
#include "nyangine-build/hooks.c"
#include "nyangine-build/test.c"
// After test.c: they borrow NYA_BuildRulePointer and its derived array, which test.c declares.
#include "nyangine-build/bench.c"
#include "nyangine-build/lint.c"
// After lint.c, whose lint_run it calls before clang-tidy.
#include "nyangine-build/check.c"
// A sibling gate: spell-checks the prose and code. Independent of the above; here beside check.c because it is the other read-only quality gate.
#include "nyangine-build/typos.c"
// Another sibling gate: bounded model checking of the untrusted-input parsers with CBMC. Beside the other read-only gates, and independent of them.
#include "nyangine-build/verify.c"
// Another sibling gate, over the commit messages rather than the code: a thin bridge onto the shared shell linter the commit-msg hook also runs.
#include "nyangine-build/commit.c"
// Another sibling gate beside check.c and typos.c: clang-format over the hand-written C. Advisory, and
// not on the critical path, so it sits with the other read-only quality gates.
#include "nyangine-build/format.c"
// Before dist.c: it defines build_capture, and writes the CHANGELOG.md every distribution ships.
#include "nyangine-build/changelog.c"
// After changelog.c: it calls build_capture, which changelog.c defines.
#include "nyangine-build/sbom.c"
#include "nyangine-build/dist.c"
#include "nyangine-build/example.c"
#include "nyangine-build/plugin.c"
// After test.c: both name the host flags and the sanitizer environment a test binary runs under.
#include "nyangine-build/fuzz.c"
#include "nyangine-build/simulation.c"
#include "nyangine-build/agent.c"
// The project scaffolder: writes a new source tree from in-source templates. No dependency on the rules above, so its place here is only that it is another code command beside them.
#include "nyangine-build/new.c"
// Beside new.c: reads a project.nya manifest and resolves it to a build plan. The first slice of consuming the engine as a vendored dependency.
#include "nyangine-build/project.c"
// Beside project.c: another .nya reader, this one round-tripping a file through the engine's serde to format or lint it.
#include "nyangine-build/fmt.c"
// Last: the commands it defines name rules and handlers from all of the above.
#include "nyangine-build/cli.c"
