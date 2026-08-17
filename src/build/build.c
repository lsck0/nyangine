#include "build/asset.c"
#include "build/i18n.c"
#include "build/reflection.c"
#include "build/hooks.c"
#include "build/test.c"
// After test.c: it borrows NYA_BuildRulePointer and its derived array, which test.c declares.
#include "build/check.c"
// After host.h has been seen via build.h: it names FLAGS_HOST_NATIVE and SANITIZER_ENVIRONMENT.
#include "build/example.c"
// Last: the commands it defines name rules and handlers from all four above.
#include "build/cli.c"
