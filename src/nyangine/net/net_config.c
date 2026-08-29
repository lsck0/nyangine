#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether `argument` is `--name` or `--name=value`, and where the value is.
 *
 * Both spellings, because both are what people type — and because the attached form is the only one
 * that is unambiguous next to a positional. The same reasoning as base_args.c, which had a live bug
 * from supporting only the separated form.
 *
 * `out_attached` is the value when it was attached, and null when the value is the next argv entry.
 * */
NYA_INTERNAL b8 _nya_net_config_matches(NYA_ConstCString argument, NYA_ConstCString name, OUT NYA_ConstCString* out_attached) __attr_no_discard;

/**
 * The value for a flag: attached if there was one, otherwise the next argv entry.
 *
 * Advances `at` past a consumed entry. Null when there is no value, which every caller treats as "keep
 * the default" rather than as an error.
 * */
NYA_INTERNAL NYA_ConstCString _nya_net_config_value(s32 argc, NYA_CString* argv, s32* at, NYA_ConstCString attached) __attr_no_discard;

/** Parses an unsigned decimal, or reports the default with a warning. Never exits. */
NYA_INTERNAL u64 _nya_net_config_number(NYA_ConstCString text, NYA_ConstCString what, u64 fallback) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NetLaunchConfig nya_net_config_from_args(s32 argc, NYA_CString* argv) {
    NYA_NetLaunchConfig config = {
        // Single player is a server with nobody listening. See net.h; this default is that claim.
        .role = NYA_NET_ROLE_SERVER,
        .port = NYA_NET_DEFAULT_PORT,
    };

    (void)snprintf(config.name, sizeof(config.name), "%s", "player");

    b8 wants_server  = false;
    b8 wants_connect = false;

    // From one, because argv[0] is the executable.
    for (s32 at = 1; at < argc; at++) {
        if (argv[at] == nullptr) continue;

        NYA_ConstCString argument = argv[at];
        NYA_ConstCString attached = nullptr;

        if (_nya_net_config_matches(argument, "server", &attached)) {
            wants_server     = true;
            config.dedicated = true;
            continue;
        }

        if (_nya_net_config_matches(argument, "connect", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            if (value == nullptr || value[0] == '\0') {
                nya_log_warn("--connect needs an address; ignoring it and starting single player.");
                continue;
            }

            wants_connect = true;
            (void)snprintf(config.address, sizeof(config.address), "%s", value);
            continue;
        }

        if (_nya_net_config_matches(argument, "port", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            u64 port = _nya_net_config_number(value, "--port", NYA_NET_DEFAULT_PORT);

            // Zero is "let the system choose", which is meaningless for a port players have to reach,
            // and anything above 65535 is not a port at all.
            if (port == 0 || port > 65535) {
                nya_log_warn("--port %llu is not a usable port; using %d.", (unsigned long long)port, NYA_NET_DEFAULT_PORT);
                port = NYA_NET_DEFAULT_PORT;
            }

            config.port = (u16)port;
            continue;
        }

        if (_nya_net_config_matches(argument, "listen", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            u64 port = _nya_net_config_number(value, "--listen", NYA_NET_DEFAULT_PORT);

            if (port == 0 || port > 65535) {
                nya_log_warn("--listen %llu is not a usable port; not listening.", (unsigned long long)port);
                continue;
            }

            config.listen_port = (u16)port;
            continue;
        }

        if (_nya_net_config_matches(argument, "name", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            if (value == nullptr || value[0] == '\0') {
                nya_log_warn("--name needs a value; keeping '%s'.", config.name);
                continue;
            }

            // Truncated rather than refused. A name is cosmetic, and a player with a long one should
            // get a short one rather than no game.
            (void)snprintf(config.name, sizeof(config.name), "%s", value);
            continue;
        }

        if (_nya_net_config_matches(argument, "max-players", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            config.max_players = (u32)_nya_net_config_number(value, "--max-players", 0);
            continue;
        }

        if (_nya_net_config_matches(argument, "tickrate", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            config.tickrate = (u32)_nya_net_config_number(value, "--tickrate", 0);
            continue;
        }

        if (_nya_net_config_matches(argument, "seed", &attached)) {
            NYA_ConstCString value = _nya_net_config_value(argc, argv, &at, attached);

            config.world_seed = _nya_net_config_number(value, "--seed", 0);
            continue;
        }

        /*
         * Anything else is ignored, at debug rather than warn.
         *
         * This is a shipped game's command line: Steam adds its own arguments, a launcher adds more,
         * and a player's stale launch option must not cost them their game. A tool that wants to
         * refuse unknown input uses base_args.h, which does exactly that.
         */
        nya_log_debug("Ignoring unrecognised launch argument '%s'.", argument);
    }

    /*
     * Contradictory. The server wins.
     *
     * A launch script naming both more likely meant to host — and the alternative, refusing to start,
     * is the worst of the three outcomes for whoever wrote it.
     */
    if (wants_server && wants_connect) {
        nya_log_warn("Both --server and --connect were given; running as a server and ignoring --connect.");
        wants_connect      = false;
        config.address[0]  = '\0';
    }

    if (wants_connect) {
        config.role      = NYA_NET_ROLE_CLIENT;
        config.dedicated = false;
    }

    // A dedicated server listens by definition: it exists for other people to connect to, so a
    // --server without a --listen would be a process nobody can reach.
    if (config.dedicated && config.listen_port == 0) config.listen_port = config.port;

    return config;
}

void nya_net_config_report(const NYA_NetLaunchConfig* config) {
    nya_assert(config != nullptr);

    if (config->role == NYA_NET_ROLE_CLIENT) {
        nya_log_info("Joining %s:%u as '%s'.", config->address, config->port, config->name);
        return;
    }

    if (config->dedicated) {
        nya_log_info("Dedicated server on port %u, up to %u players.", config->listen_port,
                 config->max_players == 0 ? NYA_NET_MAX_PEERS : config->max_players);
        return;
    }

    if (config->listen_port != 0) {
        nya_log_info("Listen server on port %u, playing as '%s'.", config->listen_port, config->name);
        return;
    }

    nya_log_info("Single player as '%s'.", config->name);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_net_config_matches(NYA_ConstCString argument, NYA_ConstCString name, OUT NYA_ConstCString* out_attached) {
    nya_assert(argument != nullptr);
    nya_assert(name != nullptr);
    nya_assert(out_attached != nullptr);

    *out_attached = nullptr;

    // Only the long form. A single-letter flag in a game's command line collides with whatever a
    // launcher prepends, and there is nothing here anyone types often enough to want the short one.
    if (argument[0] != '-' || argument[1] != '-') return false;

    const char* cursor = argument + 2;

    for (const char* expected = name; *expected != '\0'; expected++, cursor++) {
        if (*cursor != *expected) return false;
    }

    // An exact match: the value, if any, is the next argv entry.
    if (*cursor == '\0') return true;

    // `--name=value`: the value is attached. Everything after the first '=' is it, so a value
    // containing an '=' survives.
    if (*cursor == '=') {
        *out_attached = cursor + 1;
        return true;
    }

    // A longer flag that merely starts with this name — `--portable` against `--port`.
    return false;
}

NYA_ConstCString _nya_net_config_value(s32 argc, NYA_CString* argv, s32* at, NYA_ConstCString attached) {
    nya_assert(at != nullptr);

    if (attached != nullptr) return attached;

    if (*at + 1 >= argc) return nullptr;
    if (argv[*at + 1] == nullptr) return nullptr;

    /*
     * The next entry is only a value if it does not itself look like a flag.
     *
     * The same trap base_args.c had: consuming it unconditionally means `--port --server` swallows
     * `--server` and then complains that it is not a number, while the mode silently does not change.
     */
    if (argv[*at + 1][0] == '-' && argv[*at + 1][1] == '-') return nullptr;

    *at += 1;

    return argv[*at];
}

u64 _nya_net_config_number(NYA_ConstCString text, NYA_ConstCString what, u64 fallback) {
    if (text == nullptr || text[0] == '\0') {
        nya_log_warn("%s needs a number; using %llu.", what, (unsigned long long)fallback);
        return fallback;
    }

    u64 value = 0;

    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            nya_log_warn("%s '%s' is not a number; using %llu.", what, text, (unsigned long long)fallback);
            return fallback;
        }

        // Overflow, on a value that came from a command line. Reported rather than wrapped, because a
        // wrapped port number is a port nobody asked for.
        if (value > (UINT64_MAX - (u64)(*cursor - '0')) / 10) {
            nya_log_warn("%s '%s' is too large; using %llu.", what, text, (unsigned long long)fallback);
            return fallback;
        }

        value = (value * 10) + (u64)(*cursor - '0');
    }

    return value;
}
