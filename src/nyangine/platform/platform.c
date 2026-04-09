#include "nyangine/base/base.h"

#if OS_WINDOWS
#include "nyangine/platform/clock/clock_windows.c"
#include "nyangine/platform/command/command_windows.c"
#include "nyangine/platform/filesystem/filesystem_windows.c"
#include "nyangine/platform/signals/signals_windows.c"
#elif OS_LINUX
#include "nyangine/platform/clock/clock_linux.c"
#include "nyangine/platform/command/command_linux.c"
#include "nyangine/platform/filesystem/filesystem_linux.c"
#include "nyangine/platform/signals/signals_linux.c"
#else
#error "Unsupported OS"
#endif
