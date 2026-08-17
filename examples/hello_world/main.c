#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    nya_info("Hello, world!");

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
