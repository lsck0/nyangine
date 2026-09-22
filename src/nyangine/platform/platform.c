#include "nyangine/base/base.h"

// Target independent halves, before the per target sources: ipc.c declares the seven internals they
// define and terminal.c the six, and each of these holds the part of its module that is the same on
// both.
#include "nyangine/platform/clock/clock.c"
#include "nyangine/platform/clock/clock_format.c"
#include "nyangine/platform/clock/clock_instant.c"
#include "nyangine/platform/host/host.c"
#include "nyangine/platform/ipc/ipc.c"
#include "nyangine/platform/terminal/terminal.c"

#if OS_WINDOWS
#include "nyangine/platform/clock/clock_windows.c"
#include "nyangine/platform/command/command_windows.c"
#include "nyangine/platform/filesystem/filesystem_windows.c"
#include "nyangine/platform/host/host_windows.c"
#include "nyangine/platform/ipc/ipc_windows.c"
#include "nyangine/platform/memory/memory_windows.c"
#include "nyangine/platform/random/random_windows.c"
#include "nyangine/platform/signals/signals_windows.c"
#include "nyangine/platform/terminal/terminal_windows.c"
#elif OS_LINUX
#include "nyangine/platform/clock/clock_linux.c"
#include "nyangine/platform/command/command_linux.c"
#include "nyangine/platform/filesystem/filesystem_linux.c"
#include "nyangine/platform/host/host_linux.c"
#include "nyangine/platform/ipc/ipc_linux.c"
#include "nyangine/platform/memory/memory_linux.c"
#include "nyangine/platform/random/random_linux.c"
#include "nyangine/platform/signals/signals_linux.c"
#include "nyangine/platform/terminal/terminal_linux.c"
#else
#error "Unsupported OS"
#endif
