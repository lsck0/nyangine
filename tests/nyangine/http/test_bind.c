/**
 * What address the HTTP server binds when the caller names one, and when they name nothing.
 *
 * The default has to be loopback: a metrics endpoint, or any server, that is reachable off the box
 * without anyone asking for it is the kind of exposure nobody decides on purpose. This holds the one
 * place that decision is made — _nya_http_bind_address — to account, straight against known configs,
 * so it cannot drift. It binds nothing and opens no socket: the choice is a pure function of the config.
 *
 * The engine is included first, before any libc header, so base_basic.h's POSIX level is set before the
 * system headers the engine pulls in see it. Naming _nya_http_bind_address compiles this against the
 * engine's own translation unit, which is where that internal lives.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

s32 main(void) {
    // TEST: an unset address defaults to loopback, never to every interface
    {
        NYA_ConstCString bound = _nya_http_bind_address(&(NYA_HttpConfig){ 0 });

        nya_check(strcmp(bound, "127.0.0.1") == 0, "the default bind must be 127.0.0.1, is '%s'", bound);
        nya_check(nya_string_starts_with(bound, "127."), "and inside 127.0.0.0/8, the loopback range");
        nya_check(strcmp(bound, "0.0.0.0") != 0, "the default must never be the every-interface address");
    }

    // TEST: a named address is bound verbatim, so opting off loopback stays a choice the caller makes on purpose
    {
        NYA_HttpConfig config = { 0 };
        (void)snprintf(config.address, sizeof(config.address), "0.0.0.0");
        nya_check(strcmp(_nya_http_bind_address(&config), "0.0.0.0") == 0, "an every-interface bind is honoured when it is asked for");

        (void)snprintf(config.address, sizeof(config.address), "192.168.1.10");
        nya_check(strcmp(_nya_http_bind_address(&config), "192.168.1.10") == 0, "and so is any other named address");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
