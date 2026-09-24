/**
 * The launch vocabulary as a program with its own command line uses it: one flag at a time, then the
 * resolution that depends on all of them.
 *
 * `nya_net_config_from_args` is the same three calls over argv and is tested next door in
 * test_config.c; what is here is that the pieces mean the same thing when somebody else does the
 * parsing — which is what gnyame's CLI does, so that `--tickrate` cannot come to mean two things.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // TEST: the default is single player, and applying nothing leaves it there.
  {
    NYA_NetLaunchConfig config = nya_net_config_default();

    nya_check(config.role == NYA_NET_ROLE_SERVER, "single player is a server with nobody listening");
    nya_check(!config.dedicated, "and not a dedicated one");
    nya_check(config.port == NYA_NET_DEFAULT_PORT, "on the default port, got %u", config.port);
    nya_check(config.transport == NYA_NET_TRANSPORT_UDP, "over sockets, got %u", (u32)config.transport);
    nya_check(config.listen_port == 0, "listening nowhere");

    nya_net_config_finish(&config);

    nya_check(config.role == NYA_NET_ROLE_SERVER && !config.dedicated && config.listen_port == 0, "and settling changes none of that");
  }

  // TEST: one flag at a time, the way a parsed command line hands them over.
  {
    NYA_NetLaunchConfig config = nya_net_config_default();

    nya_check(nya_net_config_apply(&config, "tickrate", "60"), "tickrate is a flag this vocabulary has");
    nya_check(config.tickrate == 60, "and it took it, got %u", config.tickrate);

    nya_check(nya_net_config_apply(&config, "name", "ada"), "so is name");
    nya_check(nya_string_equals(config.name, "ada"), "got '%s'", config.name);

    nya_check(nya_net_config_apply(&config, "seed", "1234"), "and seed");
    nya_check(config.world_seed == 1234, "got " FMTu64, config.world_seed);

    nya_check(nya_net_config_apply(&config, "max-players", "8"), "and max-players");
    nya_check(config.max_players == 8, "got %u", config.max_players);

    // a flag that takes no value, which is the one exception in this vocabulary.
    nya_check(nya_net_config_apply(&config, "server", nullptr), "server takes no value");
    nya_check(config.dedicated, "and makes this dedicated");

    nya_check(!nya_net_config_apply(&config, "not-a-flag", "x"), "and something that is not a flag is refused rather than ignored");
  }

  // TEST: the resolution, which is what needs every flag rather than one.
  {
    NYA_NetLaunchConfig config = nya_net_config_default();

    (void)nya_net_config_apply(&config, "server", nullptr);
    (void)nya_net_config_apply(&config, "port", "27100");

    nya_check(config.listen_port == 0, "a server has not decided where to listen until it is settled");

    nya_net_config_finish(&config);

    // a dedicated server exists for other people to reach, so one that listened nowhere would be a process nobody can connect to.
    nya_check(config.listen_port == 27100, "and then it listens on its port, got %u", config.listen_port);

    // the contradiction: both were given, and the server wins.
    NYA_NetLaunchConfig both = nya_net_config_default();

    (void)nya_net_config_apply(&both, "server", nullptr);
    (void)nya_net_config_apply(&both, "connect", "203.0.113.9");

    nya_net_config_finish(&both);

    nya_check(both.dedicated, "a --server with a --connect is a server");
    nya_check(both.address[0] == '\0', "with the address dropped, got '%s'", both.address);
    nya_check(both.role == NYA_NET_ROLE_SERVER, "and the role to match");

    // and a client is not dedicated, whatever order the flags arrived in.
    NYA_NetLaunchConfig client = nya_net_config_default();

    (void)nya_net_config_apply(&client, "connect", "203.0.113.9");
    nya_net_config_finish(&client);

    nya_check(client.role == NYA_NET_ROLE_CLIENT, "a --connect is a client");
    nya_check(!client.dedicated, "and never dedicated");
  }

  // TEST: a value that is not usable is reported, not refused.
  {
    NYA_NetLaunchConfig config = nya_net_config_default();

    // somebody who mistyped a port wants the game to start; the warning is how they find out.
    nya_check(nya_net_config_apply(&config, "port", "not-a-number"), "the flag was still one this knows");
    nya_check(config.port == NYA_NET_DEFAULT_PORT, "and the default stands, got %u", config.port);

    nya_check(nya_net_config_apply(&config, "connect", ""), "an empty address is the flag with nothing after it");
    nya_check(config.address[0] == '\0', "and connects to nobody");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
