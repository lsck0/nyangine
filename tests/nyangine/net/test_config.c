/**
 * The launch configuration parser: one executable, four modes, decided from argv.
 *
 * This runs in a shipped game's startup path, and the input is not entirely under anyone's control —
 * Steam appends its own arguments, launchers append more, and a player's saved launch options outlive the
 * build that understood them. So the contract is unusual for a parser: it must **never fail and never
 * exit**. An unrecognised argument is ignored and a malformed value falls back to a default, because the
 * alternative is a player staring at a usage message they cannot see.
 *
 * That makes the interesting cases the degenerate ones. A parser that handles `--port 27015` is easy; the
 * failures are `--port` with nothing after it, `--port --server` where the next token is another flag,
 * `--port 99999` which is not a port, and `--port 99999999999999999999` which does not fit a u64.
 *
 * The other half is the mode logic. Single player, listen server, dedicated server and client are one
 * process distinguished by these fields, and getting the precedence wrong means a launch script that says
 * one thing does another.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Parses a literal argv, so each case reads as the command line it represents. */
#define PARSE(...)                                                                                                                                   \
  nya_net_config_from_args((s32)(sizeof((NYA_CString[]){ "gnyame", __VA_ARGS__ }) / sizeof(NYA_CString)),                                             \
                           (NYA_CString[]){ "gnyame", __VA_ARGS__ })

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  nya_backtrace_init();
  defer nya_backtrace_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: no arguments is single player
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: bare invocation is single player\n");
  {
    NYA_CString         argv[] = { "gnyame" };
    NYA_NetLaunchConfig config = nya_net_config_from_args(1, argv);

    /*
     * A server, not a fourth mode. That is the whole architecture: single player is a server with nobody
     * listening, which is why "open to LAN" is a runtime call rather than a different build.
     */
    nya_assert(config.role == NYA_NET_ROLE_SERVER, "single player is a server role");
    nya_assert(!config.dedicated, "and it is not dedicated");
    nya_assert(config.listen_port == 0, "and it is not listening");
    nya_assert(config.address[0] == '\0');
    nya_assert(config.port == NYA_NET_DEFAULT_PORT, "the port defaults even when unused");
    nya_assert(config.name[0] != '\0', "there is always a name");

    nya_net_config_report(&config);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: --server is a dedicated server, and listens by definition
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --server\n");
  {
    NYA_NetLaunchConfig config = PARSE("--server");

    nya_assert(config.role == NYA_NET_ROLE_SERVER);
    nya_assert(config.dedicated, "--server means no local player");

    /*
     * A dedicated server with no port to reach it on would be a process nobody can connect to, so the
     * listen port defaults from `port` rather than staying zero.
     */
    nya_assert(config.listen_port == NYA_NET_DEFAULT_PORT, "a dedicated server listens by definition");

    nya_net_config_report(&config);
  }

  printf("TEST: --server --port\n");
  {
    NYA_NetLaunchConfig config = PARSE("--server", "--port", "27016");

    nya_assert(config.port == 27016);
    nya_assert(config.listen_port == 27016, "the listen port follows the port on a dedicated server");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: --connect is a client
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --connect\n");
  {
    NYA_NetLaunchConfig config = PARSE("--connect", "192.168.1.5", "--port", "27017", "--name", "Luca");

    nya_assert(config.role == NYA_NET_ROLE_CLIENT);
    nya_assert(!config.dedicated, "a client is never dedicated");
    nya_assert(nya_string_equals(config.address, "192.168.1.5"));
    nya_assert(config.port == 27017);
    nya_assert(nya_string_equals(config.name, "Luca"));

    nya_net_config_report(&config);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: --listen turns single player into a listen server
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --listen\n");
  {
    NYA_NetLaunchConfig config = PARSE("--listen", "27018");

    nya_assert(config.role == NYA_NET_ROLE_SERVER);
    nya_assert(!config.dedicated, "a listen server still has a local player");
    nya_assert(config.listen_port == 27018);

    nya_net_config_report(&config);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the attached form works for every flag
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --flag=value\n");
  {
    /*
     * Both spellings, because both are what people type — and because the attached form is the only one
     * that is unambiguous next to another flag. base_args.c had a live bug from supporting only the
     * separated form, which is why this parser was written with both from the start.
     */
    NYA_NetLaunchConfig config = PARSE("--server", "--port=27019", "--name=Ada", "--max-players=8", "--tickrate=30", "--seed=12345");

    nya_assert(config.port == 27019);
    nya_assert(nya_string_equals(config.name, "Ada"));
    nya_assert(config.max_players == 8);
    nya_assert(config.tickrate == 30);
    nya_assert(config.world_seed == 12345);
  }

  printf("TEST: --connect=address\n");
  {
    NYA_NetLaunchConfig config = PARSE("--connect=example.com");

    nya_assert(config.role == NYA_NET_ROLE_CLIENT);
    nya_assert(nya_string_equals(config.address, "example.com"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a flag is not swallowed as another flag's value
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --port --server\n");
  {
    /*
     * The trap base_args.c fell into: consuming the next token unconditionally means `--port --server`
     * takes `--server` as the port, complains it is not a number, and silently does not change the mode.
     *
     * Here the next token is only a value if it does not itself look like a flag — so the port keeps its
     * default and, crucially, `--server` still takes effect.
     */
    NYA_NetLaunchConfig config = PARSE("--port", "--server");

    nya_assert(config.dedicated, "--server was swallowed as the port's value");
    nya_assert(config.port == NYA_NET_DEFAULT_PORT, "the port kept its default");
  }

  printf("TEST: a flag at the end of the line with no value\n");
  {
    NYA_NetLaunchConfig config = PARSE("--server", "--port");

    nya_assert(config.dedicated);
    nya_assert(config.port == NYA_NET_DEFAULT_PORT, "a value-less --port falls back rather than reading past argv");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: malformed and out of range values fall back
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nonsense values\n");
  {
    // Not a number at all.
    nya_assert(PARSE("--port", "banana").port == NYA_NET_DEFAULT_PORT, "a non-numeric port falls back");

    // Partly a number, which is the case a naive parser accepts as the prefix.
    nya_assert(PARSE("--port", "271x6").port == NYA_NET_DEFAULT_PORT, "a partly numeric port is refused, not truncated");

    // Zero is not a port players can reach, and above 65535 is not a port at all.
    nya_assert(PARSE("--port", "0").port == NYA_NET_DEFAULT_PORT, "port zero falls back");
    nya_assert(PARSE("--port", "99999").port == NYA_NET_DEFAULT_PORT, "a port past 65535 falls back");
    nya_assert(PARSE("--port", "65535").port == 65535, "the highest real port is accepted");

    /*
     * Past what a u64 holds.
     *
     * Reported rather than wrapped: a wrapped port number is a port nobody asked for, and it would look
     * like the parser had accepted the value.
     */
    nya_assert(PARSE("--port", "99999999999999999999999").port == NYA_NET_DEFAULT_PORT, "an overflowing number falls back");

    // An empty attached value is a value the user wrote, so it is parsed and refused rather than treated
    // as absent — falling back to the next token here would be the greedy behaviour all over again.
    nya_assert(PARSE("--port=", "27020").port == NYA_NET_DEFAULT_PORT, "an empty attached value does not reach forward");

    // A listen port that is nonsense means not listening, rather than listening somewhere arbitrary.
    nya_assert(PARSE("--listen", "0").listen_port == 0, "an unusable listen port means not listening");
    nya_assert(PARSE("--listen", "70000").listen_port == 0);

    // An empty name keeps the default rather than leaving the player nameless.
    nya_assert(PARSE("--name=").name[0] != '\0', "an empty name keeps the default");

    // A --connect with no address is not a client.
    nya_assert(PARSE("--connect").role == NYA_NET_ROLE_SERVER, "a --connect with no address stays single player");
    nya_assert(PARSE("--connect=").role == NYA_NET_ROLE_SERVER);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: contradictory arguments resolve, they do not fail
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --server and --connect together\n");
  {
    /*
     * A launch script naming both more likely meant to host, and refusing to start is the worst of the
     * three outcomes for whoever wrote it. Either order, so the answer does not depend on how it was typed.
     */
    NYA_NetLaunchConfig first  = PARSE("--server", "--connect", "10.0.0.1");
    NYA_NetLaunchConfig second = PARSE("--connect", "10.0.0.1", "--server");

    nya_assert(first.role == NYA_NET_ROLE_SERVER && first.dedicated, "--server wins");
    nya_assert(first.address[0] == '\0', "and the address is cleared, so nothing later reads it");

    nya_assert(second.role == NYA_NET_ROLE_SERVER && second.dedicated, "--server wins whichever order it was given in");
    nya_assert(second.address[0] == '\0');
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: unknown arguments are ignored, not fatal
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: unrecognised arguments\n");
  {
    /*
     * The property that makes this parser different from base_args.h. Steam adds its own arguments, a
     * launcher adds more, and a player's stale launch option must not cost them their game.
     */
    NYA_NetLaunchConfig config = PARSE("--steam-overlay", "-silent", "positional", "--server", "--unknown=7", "--port", "27021");

    nya_assert(config.dedicated, "--server still took effect around the noise");
    nya_assert(config.port == 27021, "and so did --port");
  }

  printf("TEST: a flag that merely starts with a known name\n");
  {
    // `--portable` is not `--port`. A prefix match here would set the port from an unrelated flag's value.
    NYA_NetLaunchConfig config = PARSE("--portable", "27022");

    nya_assert(config.port == NYA_NET_DEFAULT_PORT, "--portable was matched as --port");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: overlong values are truncated rather than overflowing
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: overlong name and address\n");
  {
    /*
     * Both are copied into fixed buffers. A name is cosmetic, so a player with a long one gets a short one
     * rather than no game — but the copy must not run past the buffer, which is what this is really for.
     */
    char long_name[512];
    nya_memset(long_name, 'N', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    NYA_NetLaunchConfig config = nya_net_config_from_args(3, (NYA_CString[]){ "gnyame", "--name", long_name });

    nya_assert(strlen(config.name) < NYA_NET_MAX_NAME, "the name was truncated to its buffer");
    nya_assert(config.name[0] == 'N');

    char long_address[512];
    nya_memset(long_address, 'a', sizeof(long_address) - 1);
    long_address[sizeof(long_address) - 1] = '\0';

    NYA_NetLaunchConfig client = nya_net_config_from_args(3, (NYA_CString[]){ "gnyame", "--connect", long_address });

    nya_assert(client.role == NYA_NET_ROLE_CLIENT);
    nya_assert(strlen(client.address) < NYA_NET_MAX_ADDRESS, "the address was truncated to its buffer");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a null entry in argv is skipped
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a null argv entry\n");
  {
    // Legal to hand over and produced by some launchers. Dereferencing it would be a crash before the
    // game drew a frame.
    NYA_CString         argv[] = { "gnyame", nullptr, "--server", nullptr };
    NYA_NetLaunchConfig config = nya_net_config_from_args(4, argv);

    nya_assert(config.dedicated, "--server was lost around the nulls");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: random argv never faults
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: random argv\n");
  {
    /*
     * The catch-all. Anything can appear on a command line, including bytes nobody typed — and the
     * contract is that this always returns a usable config.
     */
    NYA_RNG             rng     = nya_rng_create(.seed = "C0FFEE");
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 32.0, .max = 126.0 } };

    for (u32 iteration = 0; iteration < 4000; iteration++) {
      char        storage[8][64];
      NYA_CString argv[9] = { "gnyame" };

      u32 count = 1 + (iteration % 8);

      for (u32 i = 0; i < count; i++) {
        u32 length = 1 + (iteration + i) % 40;

        // A third of them start with `--`, so the flag-matching path is reached rather than every token
        // being dismissed as a positional.
        u32 at = 0;
        if ((iteration + i) % 3 == 0) {
          storage[i][at++] = '-';
          storage[i][at++] = '-';
        }

        for (; at < length; at++) storage[i][at] = (char)nya_rng_sample_u8(&rng, uniform);

        storage[i][at] = '\0';
        argv[1 + i]    = storage[i];
      }

      NYA_NetLaunchConfig config = nya_net_config_from_args((s32)(1 + count), argv);

      // Whatever it decided, the result has to be usable: a valid role, a port that is a port, and a
      // name that is a terminated string inside its buffer.
      nya_assert(config.role == NYA_NET_ROLE_SERVER || config.role == NYA_NET_ROLE_CLIENT, "iteration %u produced role %d", iteration,
                 (int)config.role);
      nya_assert(config.port > 0, "iteration %u produced port %u", iteration, config.port);
      nya_assert(strlen(config.name) < NYA_NET_MAX_NAME, "iteration %u overran the name buffer", iteration);
      nya_assert(strlen(config.address) < NYA_NET_MAX_ADDRESS, "iteration %u overran the address buffer", iteration);

      // A client always has an address, because that is what made it a client.
      if (config.role == NYA_NET_ROLE_CLIENT) nya_assert(config.address[0] != '\0', "iteration %u is a client with no address", iteration);
    }

    printf("  4000 random command lines all produced a usable config\n");
  }

  printf("PASSED: test_config (0 failures)\n");

  return EXIT_SUCCESS;
}
