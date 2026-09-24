#include "nyangine/nyangine.h"

// PRIVATE API DECLARATION

/** Whether `name` is a flag that takes a value; `--server` is the only one that takes none. */
NYA_INTERNAL b8 _nya_net_config_takes_value(NYA_ConstCString name) __attr_no_discard;

/** Parses an unsigned decimal, or reports the default with a warning. Never exits. */
NYA_INTERNAL u64 _nya_net_config_number(NYA_ConstCString text, NYA_ConstCString what, u64 fallback) __attr_no_discard;

/** Parses a percentage, 0..100 with a fraction allowed, or reports zero with a warning. */
NYA_INTERNAL f32 _nya_net_config_percent(NYA_ConstCString text, NYA_ConstCString what) __attr_no_discard;

/** Whether every byte of `text` could appear in a hostname, an IPv4 literal or a bare IPv6 literal. */
NYA_INTERNAL b8 _nya_net_config_address_is_plausible(NYA_ConstCString text, u64 length) __attr_no_discard;

/** Parses exactly `length` decimal digits as a port, 1..65535. Zero means it was not one. */
NYA_INTERNAL u16 _nya_net_config_port_from_text(NYA_ConstCString text, u64 length) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_NetLaunchConfig nya_net_config_default(void) {
    NYA_NetLaunchConfig config = {
        // Single player is a server with nobody listening. See net.h; this default is that claim.
        .role = NYA_NET_ROLE_SERVER,
        .port = NYA_NET_DEFAULT_PORT,

        // Sockets unless something says otherwise: loopback (the enum's zero) has no second end and a command line cannot ask for it.
        .transport = NYA_NET_TRANSPORT_UDP,
    };

    (void)snprintf(config.name, sizeof(config.name), "%s", "player");

    return config;
}

b8 nya_net_config_apply(NYA_NetLaunchConfig* config, NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(config != nullptr);
    nya_assert(name != nullptr);

    if (nya_string_equals(name, "server")) {
        config->dedicated = true;
        return true;
    }

    if (nya_string_equals(name, "connect")) {
        if (value == nullptr || value[0] == '\0') {
            nya_log_warn("--connect needs an address; ignoring it and starting single player.");
            return true;
        }

        (void)snprintf(config->address, sizeof(config->address), "%s", value);
        return true;
    }

    if (nya_string_equals(name, "port")) {
        u64 port = _nya_net_config_number(value, "--port", NYA_NET_DEFAULT_PORT);

        // Zero ("let the system choose") is meaningless for a port players must reach, and above 65535 is not a port.
        if (port == 0 || port > 65535) {
            nya_log_warn("--port %llu is not a usable port; using %d.", (unsigned long long)port, NYA_NET_DEFAULT_PORT);
            port = NYA_NET_DEFAULT_PORT;
        }

        config->port = (u16)port;
        return true;
    }

    if (nya_string_equals(name, "listen")) {
        u64 port = _nya_net_config_number(value, "--listen", NYA_NET_DEFAULT_PORT);

        if (port == 0 || port > 65535) {
            nya_log_warn("--listen %llu is not a usable port; not listening.", (unsigned long long)port);
        return true;
        }

        config->listen_port = (u16)port;
        return true;
    }

    if (nya_string_equals(name, "name")) {
        if (value == nullptr || value[0] == '\0') {
            nya_log_warn("--name needs a value; keeping '%s'.", config->name);
        return true;
        }

        // Truncated rather than refused: a name is cosmetic, so a long one is shortened rather than rejected.
        (void)snprintf(config->name, sizeof(config->name), "%s", value);
        config->named = true;
        return true;
    }

    if (nya_string_equals(name, "max-players")) {
        config->max_players = (u32)_nya_net_config_number(value, "--max-players", 0);
        return true;
    }

    if (nya_string_equals(name, "tickrate")) {
        config->tickrate = (u32)_nya_net_config_number(value, "--tickrate", 0);
        return true;
    }

    if (nya_string_equals(name, "server-key")) {
        if (!nya_net_key_from_hex(value, config->server_key)) nya_log_warn("--server-key needs 64 hex digits; trusting the first key the server presents.");
        return true;
    }

    if (nya_string_equals(name, "net-latency")) {
        config->conditions.latency_ms = (u32)nya_min(_nya_net_config_number(value, "--net-latency", 0), (u64)5000);
        return true;
    }

    if (nya_string_equals(name, "net-jitter")) {
        config->conditions.jitter_ms = (u32)nya_min(_nya_net_config_number(value, "--net-jitter", 0), (u64)5000);
        return true;
    }

    if (nya_string_equals(name, "net-loss")) {
        config->conditions.loss_percent = _nya_net_config_percent(value, "--net-loss");
        return true;
    }

    if (nya_string_equals(name, "net-duplicate")) {
        config->conditions.duplicate_percent = _nya_net_config_percent(value, "--net-duplicate");
        return true;
    }

    if (nya_string_equals(name, "net-reorder")) {
        config->conditions.reorder_percent = _nya_net_config_percent(value, "--net-reorder");
        return true;
    }

    if (nya_string_equals(name, "transport")) {
        if (value != nullptr && nya_string_equals(value, NYA_NET_JOIN_SCHEME_STEAM)) {
            config->transport = NYA_NET_TRANSPORT_STEAM;
        return true;
        }

        if (value != nullptr && nya_string_equals(value, NYA_NET_JOIN_SCHEME_UDP)) {
            config->transport = NYA_NET_TRANSPORT_UDP;
        return true;
        }

        nya_log_warn("--transport takes '%s' or '%s'; using udp.", NYA_NET_JOIN_SCHEME_UDP, NYA_NET_JOIN_SCHEME_STEAM);
        return true;
    }

    if (nya_string_equals(name, "seed")) {
        config->world_seed = _nya_net_config_number(value, "--seed", 0);
        return true;
    }

    return false;
}


void nya_net_config_finish(NYA_NetLaunchConfig* config) {
    nya_assert(config != nullptr);

    b8 wants_connect = config->address[0] != '\0';

    // Contradictory: the server wins.
    if (config->dedicated && wants_connect) {
        nya_log_warn("Both --server and --connect were given; running as a server and ignoring --connect.");

        wants_connect     = false;
        config->address[0] = '\0';
    }

    if (wants_connect) {
        config->role      = NYA_NET_ROLE_CLIENT;
        config->dedicated = false;
    }

    // A dedicated server listens by definition, so a --server without a --listen would be unreachable.
    if (config->dedicated && config->listen_port == 0) config->listen_port = config->port;
}

NYA_NetLaunchConfig nya_net_config_from_args(s32 argc, NYA_CString* argv) {
    NYA_NetLaunchConfig config = nya_net_config_default();

    // From one, because argv[0] is the executable.
    for (s32 at = 1; at < argc; at++) {
        if (argv[at] == nullptr) continue;

        NYA_ConstCString argument = argv[at];
        NYA_ConstCString attached = nullptr;

        // The vocabulary lives in nya_net_config_apply; this loop only finds the pairs to hand it.
        char name[64] = { 0 };

        u64 length = 0;
        while (argument[length] != '\0' && argument[length] != '=') length++;

        if (length < 3 || argument[0] != '-' || argument[1] != '-') {
            nya_log_debug("Ignoring unrecognised launch argument '%s'.", argument);
            continue;
        }

        u64 copied = length - 2 < sizeof(name) - 1 ? length - 2 : sizeof(name) - 1;
        nya_memcpy(name, argument + 2, copied);

        if (argument[length] == '=') attached = argument + length + 1;

        // The value is read lazily as the next entry, but only when it is not itself a flag: `--port --server` is a port with no value.
        NYA_ConstCString next = at + 1 < argc && argv[at + 1] != nullptr ? argv[at + 1] : nullptr;

        if (next != nullptr && next[0] == '-' && next[1] == '-') next = nullptr;

        NYA_ConstCString value = attached != nullptr ? attached : next;

        if (!nya_net_config_apply(&config, name, value)) {
            nya_log_debug("Ignoring unrecognised launch argument '%s'.", argument);
            continue;
        }

        // Only a flag that actually took the next entry moves the cursor past it.
        if (attached == nullptr && value != nullptr && _nya_net_config_takes_value(name)) at++;
    }

    nya_net_config_finish(&config);

    return config;
}

void nya_net_config_report(const NYA_NetLaunchConfig* config) {
    nya_assert(config != nullptr);

    const NYA_NetConditions* bad = &config->conditions;

    if (nya_net_conditions_active(*bad)) {
        nya_log_warn("Simulating a bad network: %u ms latency, %u ms jitter, %.1f%% loss, %.1f%% duplicated, %.1f%% reordered.", bad->latency_ms, bad->jitter_ms,
                     (f64)bad->loss_percent, (f64)bad->duplicate_percent, (f64)bad->reorder_percent);
    }

    if (config->role == NYA_NET_ROLE_CLIENT) {
        if (config->transport == NYA_NET_TRANSPORT_STEAM) {
            nya_log_info("Joining Steam account %s as '%s'.", config->address, config->name);
            return;
        }

        nya_log_info("Joining %s:%u as '%s'.", config->address, config->port, config->name);
        return;
    }

    if (config->transport == NYA_NET_TRANSPORT_STEAM) {
        nya_log_info("Listen server over Steam, playing as '%s'.", config->name);
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

// JOIN SECRETS

b8 nya_net_config_to_join_secret(const NYA_NetLaunchConfig* config, OUT char* out_secret, u64 capacity) {
    nya_assert(config != nullptr);
    nya_assert(out_secret != nullptr);
    nya_assert(capacity > 0);

    out_secret[0] = '\0';

    b8 steam = config->transport == NYA_NET_TRANSPORT_STEAM;

    // Which of the two ports is the one a friend would dial.
    u16 port = config->role == NYA_NET_ROLE_CLIENT ? config->port : config->listen_port;

    // over Steam there is no port to dial, so the default stands in and the secret keeps its five fixed fields.
    if (steam) port = port == 0 ? NYA_NET_DEFAULT_PORT : port;

    // single player has no port open, so nothing to invite to; reported, not asserted, since it is the ordinary case.
    if (port == 0) return false;

    NYA_ConstCString address = config->address;

    // a listen server does not guess its own public address; the provider substitutes its own routing.
    if (config->role != NYA_NET_ROLE_CLIENT) address = "";

    char steam_address[24] = { 0 };

    // over Steam the host does know how to be reached, because the address is its own account.
    if (steam && address[0] == '\0') {
        (void)snprintf(steam_address, sizeof(steam_address), "%llu", (unsigned long long)nya_steam_user_id().value);

        if (steam_address[0] == '0') return false;

        address = steam_address;
    }

    NYA_ConstCString scheme = steam ? NYA_NET_JOIN_SCHEME_STEAM : NYA_NET_JOIN_SCHEME_UDP;

    // no key over Steam: the account is the identity and pinning is a UDP concept.
    char hex[NYA_NET_KEY_HEX_SIZE] = { 0 };
    b8   keyed                     = !steam && nya_net_key_is_set(config->server_key);

    if (keyed) nya_net_key_to_hex(config->server_key, hex);

    s32 written = keyed ? snprintf(out_secret, capacity, NYA_NET_JOIN_SECRET_TAG "%s:%s:%u:%s", scheme, address, port, hex)
                        : snprintf(out_secret, capacity, NYA_NET_JOIN_SECRET_TAG "%s:%s:%u", scheme, address, port);

    // truncated: a half secret parses as a different address, so it is dropped rather than sent.
    if (written < 0 || (u64)written >= capacity) {
        out_secret[0] = '\0';
        return false;
    }

    return true;
}

b8 nya_net_config_from_join_secret(NYA_ConstCString secret, OUT NYA_NetLaunchConfig* out_config) {
    nya_assert(out_config != nullptr);

    *out_config = (NYA_NetLaunchConfig){ 0 };

    if (secret == nullptr) return false;

    // Bounded before anything else reads it: everything below indexes inside this length.
    u64 length = strnlen(secret, NYA_NET_MAX_JOIN_SECRET);
    if (length == 0 || length >= NYA_NET_MAX_JOIN_SECRET) return false;

    u64 tag_length = sizeof(NYA_NET_JOIN_SECRET_TAG) - 1;
    if (length <= tag_length) return false;
    if (strncmp(secret, NYA_NET_JOIN_SECRET_TAG, tag_length) != 0) return false;

    // The scheme first, from the left: it is a fixed word with no colon in it.
    u64 scheme_end = tag_length;
    while (scheme_end < length && secret[scheme_end] != ':') scheme_end++;

    if (scheme_end == length) return false;

    u64 scheme_length = scheme_end - tag_length;

    NYA_NetTransportKind transport = NYA_NET_TRANSPORT_KIND_COUNT;

    if (scheme_length == sizeof(NYA_NET_JOIN_SCHEME_UDP) - 1 && strncmp(secret + tag_length, NYA_NET_JOIN_SCHEME_UDP, scheme_length) == 0) {
        transport = NYA_NET_TRANSPORT_UDP;
    }

    if (scheme_length == sizeof(NYA_NET_JOIN_SCHEME_STEAM) - 1 && strncmp(secret + tag_length, NYA_NET_JOIN_SCHEME_STEAM, scheme_length) == 0) {
        transport = NYA_NET_TRANSPORT_STEAM;
    }

    // a scheme this build has no transport for; refused, not defaulted to UDP, which would dial the wrong thing.
    if (transport == NYA_NET_TRANSPORT_KIND_COUNT) return false;

    // Then from the right: an IPv6 literal is full of colons, so the address is what remains after the fixed tail fields.
    u64 body_start = scheme_end + 1;
    u64 body_end   = length;

    if (body_start >= body_end) return false;

    u8 server_key[NYA_NET_KEY_SIZE] = { 0 };
    b8 keyed                        = false;

    // The optional key first, since it is the last field when it is there.
    u64 last_colon = body_end;
    for (u64 i = body_end; i > body_start; i--) {
        if (secret[i - 1] == ':') {
            last_colon = i - 1;
            break;
        }
    }

    if (last_colon == body_end) return false;

    if (body_end - last_colon - 1 == NYA_NET_KEY_HEX_SIZE - 1) {
        char hex[NYA_NET_KEY_HEX_SIZE];
        nya_memcpy(hex, secret + last_colon + 1, NYA_NET_KEY_HEX_SIZE - 1);
        hex[NYA_NET_KEY_HEX_SIZE - 1] = '\0';

        // a 64 character tail that is not hex is not a key, so the secret is refused rather than read as a long port.
        if (!nya_net_key_from_hex(hex, server_key)) return false;

        keyed    = true;
        body_end = last_colon;

        last_colon = body_end;
        for (u64 i = body_end; i > body_start; i--) {
            if (secret[i - 1] == ':') {
                last_colon = i - 1;
                break;
            }
        }

        if (last_colon == body_end) return false;
    }

    u16 port = _nya_net_config_port_from_text(secret + last_colon + 1, body_end - last_colon - 1);
    if (port == 0) return false;

    u64 address_length = last_colon - body_start;
    if (address_length >= NYA_NET_MAX_ADDRESS) return false;
    if (!_nya_net_config_address_is_plausible(secret + body_start, address_length)) return false;

    // an empty address only means something over UDP; over Steam the address is the account to dial.
    if (address_length == 0 && transport == NYA_NET_TRANSPORT_STEAM) return false;

    out_config->role      = NYA_NET_ROLE_CLIENT;
    out_config->port      = port;
    out_config->transport = transport;

    if (address_length > 0) {
        nya_memcpy(out_config->address, secret + body_start, address_length);
        out_config->address[address_length] = '\0';
    }

    if (keyed) nya_memcpy(out_config->server_key, server_key, NYA_NET_KEY_SIZE);

    (void)snprintf(out_config->name, sizeof(out_config->name), "%s", "player");

    return true;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_net_config_address_is_plausible(NYA_ConstCString text, u64 length) {
    nya_assert(text != nullptr);

    // an empty address is legal and means "the provider routes this" (a Steam lobby, a Discord invite).
    for (u64 i = 0; i < length; i++) {
        char character = text[i];

        b8 letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        b8 digit  = character >= '0' && character <= '9';

        // the full set a hostname, IPv4 or bare IPv6 literal needs; a '/' or '%' would be a path or scope id.
        if (!letter && !digit && character != '.' && character != '-' && character != ':') return false;
    }

    return true;
}

u16 _nya_net_config_port_from_text(NYA_ConstCString text, u64 length) {
    nya_assert(text != nullptr);

    // five digits is 65535, so nothing longer can be a port and nothing here can overflow.
    if (length == 0 || length > 5) return 0;

    u32 value = 0;

    for (u64 i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;

        value = (value * 10) + (u32)(text[i] - '0');
    }

    if (value == 0 || value > 65535) return 0;

    return (u16)value;
}

b8 _nya_net_config_takes_value(NYA_ConstCString name) {
    return !nya_string_equals(name, "server");
}

f32 _nya_net_config_percent(NYA_ConstCString text, NYA_ConstCString what) {
    char* end   = nullptr;
    f32   value = text == nullptr ? NAN : strtof(text, &end);

    if (text == nullptr || end == text || *end != '\0' || !(value >= 0.0F && value <= 100.0F)) {
        nya_log_warn("%s needs a percentage from 0 to 100; using 0.", what);
        return 0.0F;
    }

    return value;
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

        // Overflow on a command-line value: reported rather than wrapped, since a wrapped port is one nobody asked for.
        if (value > (UINT64_MAX - (u64)(*cursor - '0')) / 10) {
            nya_log_warn("%s '%s' is too large; using %llu.", what, text, (unsigned long long)fallback);
            return fallback;
        }

        value = (value * 10) + (u64)(*cursor - '0');
    }

    return value;
}
