// The web platform primitives, as one translation unit. Each file carries its own OS_WASM backend and
// its native fallback, so this same include builds in the native tree (through platform/platform.c) and
// in a wasm module (through src/web/wasm_demo.c, beside os/os_wasm.c). They share no state and depend on
// nothing of each other; the order here is only alphabetical.
#include "nyangine/platform/web/web_clock.c"
#include "nyangine/platform/web/web_fetch.c"
#include "nyangine/platform/web/web_random.c"
#include "nyangine/platform/web/web_socket.c"
#include "nyangine/platform/web/web_storage.c"
