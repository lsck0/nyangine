#include "nyangine-std/base/base.h"

// Target independent halves, before the per target sources: ipc.c declares the seven internals they
// define and terminal.c the six, and each of these holds the part of its module that is the same on
// both.
#include "nyangine-std/platform/host/host.c"
#include "nyangine-std/platform/ipc/ipc.c"
#include "nyangine-std/platform/terminal/terminal.c"

// The web primitives are target-independent in the same sense: each file carries its OS_WASM backend and
// its native fallback, so this builds the fallbacks here and the browser backends under a wasm module.
#include "nyangine-std/platform/web/web.c"

#if OS_WINDOWS
#include "nyangine-std/platform/host/host_windows.c"
#include "nyangine-std/platform/ipc/ipc_windows.c"
#include "nyangine-std/platform/signals/signals_windows.c"
#include "nyangine-std/platform/terminal/terminal_windows.c"
#elif OS_LINUX
#include "nyangine-std/platform/host/host_linux.c"
#include "nyangine-std/platform/ipc/ipc_linux.c"
#include "nyangine-std/platform/signals/signals_linux.c"
#include "nyangine-std/platform/terminal/terminal_linux.c"
#else
#error "Unsupported OS"
#endif
