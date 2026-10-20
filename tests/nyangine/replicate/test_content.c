/**
 * Content manifests: replicate_content.h on its own, then over a real UDP handshake. A matching manifest is
 * admitted; a different plugin digest, an extra plugin and a missing one are refused with NYA_NET_DISCONNECT_CONTENT
 * and the entry named; a client-only plugin is ignored when the server allows it. Then the command action mask and
 * the game event bound, which a session over the same handshake reaches.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

#define FLAG_REPLICATED (1ULL << 2)

#define PUMP_TIMEOUT_MS 5000

/** One fixed tick, matching the engine's default. */
#define TICK_SECONDS (1.0F / 60.0F)

/** The two action bits the game below numbers; anything else in a command is a lie. */
#define ACTION_MASK 0x3ULL

/** A bit the game never registered. */
#define ACTION_UNKNOWN (1ULL << 9)

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    if (nya_net_command_holds(command, 0)) entity->position.x += 10.0F * delta_time_s;
}

static u64 HELD_ACTIONS = 0;

static void sample_command(OUT NYA_NetCommand* command) {
    command->actions = HELD_ACTIONS;
}

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
    nya_unused(peer, name);
    return nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED);
}

/* FIXTURES */

/** One entry, built the way a caller would. */
static NYA_NetContentEntry entry_make(NYA_NetContentKind kind, NYA_ConstCString id, NYA_ConstCString version, u8 digest_byte, b8 client_only) {
    NYA_NetContentEntry entry = { .kind = kind, .client_only = client_only };
    (void)snprintf(entry.id, sizeof(entry.id), "%s", id);
    (void)snprintf(entry.version, sizeof(entry.version), "%s", version);
    nya_memset(entry.digest.bytes, digest_byte, sizeof(entry.digest.bytes));
    return entry;
}

/** The build, a baked bundle and the `hello` plugin, whose digest is `hello_digest`. */
static NYA_NetContentManifest manifest_base(u8 hello_digest) {
    NYA_NetContentManifest manifest = { 0 };
    NYA_EXPECT(nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_PLUGIN, "hello", "0.1.0", hello_digest, false)));
    NYA_EXPECT(nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_ASSETS, NYA_NET_CONTENT_ID_ASSETS, NYA_NET_CONTENT_ASSETS_BAKED, 0xA5, false)));
    NYA_EXPECT(nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_BUILD, NYA_NET_CONTENT_ID_BUILD, "1.0.0+abc", 0x00, false)));
    return manifest;
}

/** Runs both sides once, the way the app loop would. */
static void run_tick(u64* tick, b8 with_client) {
    nya_net_server_tick(*tick, TICK_SECONDS);
    if (with_client) nya_net_client_tick(*tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    (*tick)++;
}

/** Pumps until the client is in the game or has been dropped. */
static NYA_NetClientState handshake_pump(u64* tick) {
    u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

    // the first ticks are CONNECTING before the transport is up, so the loop waits for HANDSHAKING to have passed.
    b8 started = false;

    while (nya_clock_get_monotonic_ms() < deadline) {
        run_tick(tick, true);

        NYA_NetClientState state = nya_net_client_state();
        if (state == NYA_NET_CLIENT_HANDSHAKING) started = true;
        if (state == NYA_NET_CLIENT_PLAYING) return state;
        if (started && state == NYA_NET_CLIENT_DISCONNECTED) return state;

        sleep_ms(2);
    }

    return nya_net_client_state();
}

/** A server with `server_content`, a UDP client with `client_content`, and the handshake between them. */
static NYA_NetClientState session_begin(u64* tick, const NYA_NetContentManifest* server_content, const NYA_NetContentManifest* client_content, b8 allow_client_only) {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
        .replicated_flag           = FLAG_REPLICATED,
        .on_apply_command          = nya_callback(apply_movement),
        .on_spawn_player           = nya_callback(spawn_player),
        .action_mask               = ACTION_MASK,
        .content                   = server_content,
        .allow_client_only_plugins = allow_client_only,
    }));

    NYA_EXPECT(nya_net_server_listen(0), "the system had no free UDP port");

    NYA_EXPECT(nya_net_client_connect("127.0.0.1", nya_net_server_port(), "modder", (NYA_NetClientConfig){
        .replicated_flag   = FLAG_REPLICATED,
        .on_apply_command  = nya_callback(apply_movement),
        .on_sample_command = nya_callback(sample_command),
        .content           = client_content,
    }));

    return handshake_pump(tick);
}

static void session_end(void) {
    nya_net_client_disconnect();
    nya_net_server_stop();

    nya_entity_foreach (leftover) nya_entity_despawn_deferred(leftover->handle);
    nya_system_sim_apply_commands();
}

/* THE SUITE */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);

    NYA_Arena* arena = nya_arena_create(.name = "test_content");
    defer      nya_arena_destroy(arena);

    u64 tick = 1;

    printf("TEST: a manifest keeps its entries in order and refuses a repeat\n");
    {
        NYA_NetContentManifest manifest = manifest_base(0x11);

        nya_assert_eq(manifest.entry_count, 3U);
        nya_assert(manifest.entries[0].kind == NYA_NET_CONTENT_BUILD, "the build sorts first");
        nya_assert(manifest.entries[1].kind == NYA_NET_CONTENT_ASSETS);
        nya_assert(manifest.entries[2].kind == NYA_NET_CONTENT_PLUGIN);

        NYA_Error repeated = nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_PLUGIN, "hello", "9.9.9", 0x22, false));
        nya_assert(repeated.kind == NYA_ERROR_ALREADY_EXISTS, "a second 'hello' was added");

        NYA_Error unprintable = nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_PLUGIN, "bad\x1b[2J", "1.0.0", 0x22, false));
        nya_assert(unprintable.kind == NYA_ERROR_INVALID_ARGUMENT, "an id with an escape sequence was added");

        nya_assert_eq(manifest.entry_count, 3U);

        // add and remove are inverses: what is left equals what was there before.
        NYA_NetContentManifest before = manifest;
        NYA_EXPECT(nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_PLUGIN, "aimbot", "6.6.6", 0x66, false)));
        nya_assert(nya_net_content_remove(&manifest, NYA_NET_CONTENT_PLUGIN, "aimbot"));
        nya_assert(!nya_net_content_remove(&manifest, NYA_NET_CONTENT_PLUGIN, "aimbot"), "an entry was removed twice");
        nya_assert(nya_memcmp(&manifest, &before, sizeof(before)) == 0, "add then remove changed the manifest");

        NYA_NetContentManifest collected = { 0 };
        NYA_EXPECT(nya_net_content_collect(&collected));
        nya_assert(collected.entry_count >= 2, "this process has at least a build and an asset entry");
        nya_assert(nya_string_equals(collected.entries[0].id, NYA_NET_CONTENT_ID_BUILD));

        printf("  sorted by kind then id, repeats and escapes refused, and this process collects %u entries\n", collected.entry_count);
    }

    printf("TEST: the hash leaves client-only entries out\n");
    {
        NYA_NetContentManifest plain    = manifest_base(0x11);
        NYA_NetContentManifest cosmetic = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&cosmetic, entry_make(NYA_NET_CONTENT_PLUGIN, "hud_skin", "2.0.0", 0x33, true)));

        NYA_CryptoSha256Digest a = { 0 };
        NYA_CryptoSha256Digest b = { 0 };
        nya_net_content_hash(&plain, &a);
        nya_net_content_hash(&cosmetic, &b);
        nya_assert(nya_memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0, "a client-only plugin moved the hash");

        NYA_NetContentManifest changed = manifest_base(0x12);
        nya_net_content_hash(&changed, &b);
        nya_assert(nya_memcmp(a.bytes, b.bytes, sizeof(a.bytes)) != 0, "a different plugin digest hashed the same");

        printf("  a cosmetic plugin leaves the hash alone, a changed digest does not\n");
    }

    printf("TEST: encode and decode round trip, and decode refuses what encode never writes\n");
    {
        NYA_NetContentManifest manifest = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&manifest, entry_make(NYA_NET_CONTENT_PLUGIN, "hud_skin", "2.0.0", 0x33, true)));

        NYA_String* encoded = nya_string_create(arena);
        nya_net_content_encode(encoded, &manifest);
        nya_assert(encoded->length <= NYA_NET_CONTENT_ENCODED_MAX);

        NYA_NetContentManifest decoded = { 0 };
        NYA_EXPECT(nya_net_content_decode(encoded->items, encoded->length, &decoded));
        nya_assert_eq(decoded.entry_count, manifest.entry_count);
        nya_assert(nya_memcmp(&decoded, &manifest, sizeof(manifest)) == 0, "the round trip changed the manifest");

        // the claimed hash is checked against the entries: a client cannot send one hash and other entries.
        encoded->items[0] ^= 0x01;
        nya_assert(nya_net_content_decode(encoded->items, encoded->length, &decoded).kind == NYA_ERROR_CORRUPT, "a forged hash decoded");
        encoded->items[0] ^= 0x01;

        nya_assert(!nya_net_content_decode(encoded->items, encoded->length - 1, &decoded).ok, "a truncated manifest decoded");

        nya_string_push_back(encoded, 0x00);
        nya_assert(!nya_net_content_decode(encoded->items, encoded->length, &decoded).ok, "a manifest with a trailing byte decoded");

        printf("  equal after the trip; a forged hash, a truncation and a trailing byte refused\n");
    }

    printf("TEST: the fuzz corpus seeds are manifests, so the fuzzer starts past the hash check\n");
    {
        NYA_String* seed = nya_string_create(arena);
        NYA_EXPECT(nya_file_read("./tests/fuzz/corpus/net_content/with_cosmetic.bin", seed));

        NYA_NetContentManifest decoded = { 0 };
        NYA_EXPECT(nya_net_content_decode(seed->items, seed->length, &decoded));
        nya_assert_eq(decoded.entry_count, 4U);
        nya_assert(decoded.entries[3].client_only && nya_string_equals(decoded.entries[3].id, "hud_skin"));
    }

    printf("TEST: compare names the first differing entry\n");
    {
        NYA_NetContentManifest server = manifest_base(0x11);

        NYA_NetContentMismatch same = nya_net_content_compare(&server, &server, (NYA_NetContentPolicy){ 0 });
        nya_assert(same.difference == NYA_NET_CONTENT_SAME);

        NYA_NetContentManifest digest = manifest_base(0x12);
        NYA_NetContentMismatch why    = nya_net_content_compare(&server, &digest, (NYA_NetContentPolicy){ 0 });
        nya_assert(why.difference == NYA_NET_CONTENT_DIGEST && nya_string_equals(why.id, "hello"), "a digest change was not named");

        NYA_NetContentManifest extra = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&extra, entry_make(NYA_NET_CONTENT_PLUGIN, "aimbot", "6.6.6", 0x66, false)));
        why = nya_net_content_compare(&server, &extra, (NYA_NetContentPolicy){ 0 });
        nya_assert(why.difference == NYA_NET_CONTENT_EXTRA && nya_string_equals(why.id, "aimbot"), "an extra plugin was not named");
        nya_assert(nya_string_equals(why.client_version, "6.6.6") && why.server_version[0] == '\0');

        why = nya_net_content_compare(&extra, &server, (NYA_NetContentPolicy){ 0 });
        nya_assert(why.difference == NYA_NET_CONTENT_MISSING && nya_string_equals(why.id, "aimbot"), "a missing plugin was not named");

        NYA_NetContentManifest older = { 0 };
        NYA_EXPECT(nya_net_content_add(&older, entry_make(NYA_NET_CONTENT_BUILD, NYA_NET_CONTENT_ID_BUILD, "0.9.0+old", 0x00, false)));
        why = nya_net_content_compare(&server, &older, (NYA_NetContentPolicy){ 0 });
        nya_assert(why.difference == NYA_NET_CONTENT_VERSION && why.kind == NYA_NET_CONTENT_BUILD, "the build is named before any plugin");

        NYA_NetContentManifest cosmetic = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&cosmetic, entry_make(NYA_NET_CONTENT_PLUGIN, "hud_skin", "2.0.0", 0x33, true)));
        nya_assert(nya_net_content_compare(&server, &cosmetic, (NYA_NetContentPolicy){ .allow_client_only = true }).difference == NYA_NET_CONTENT_SAME,
                   "an allowed client-only plugin was counted");
        nya_assert(nya_net_content_compare(&server, &cosmetic, (NYA_NetContentPolicy){ 0 }).difference == NYA_NET_CONTENT_EXTRA,
                   "a client-only plugin passed a server that does not allow them");

        char line[NYA_NET_CONTENT_DESCRIBE_MAX];
        (void)nya_net_content_mismatch_describe(&why, line, sizeof(line));
        printf("  digest, extra, missing and version each named; e.g. \"%s\"\n", line);
    }

    printf("TEST: a matching manifest is admitted over UDP\n");
    {
        NYA_NetContentManifest content = manifest_base(0x11);

        NYA_NetClientState state = session_begin(&tick, &content, &content, false);
        nya_assert(state == NYA_NET_CLIENT_PLAYING, "a client with the server's content was not admitted");
        nya_assert(nya_net_client_content_mismatch() == nullptr);

        session_end();
    }

    printf("TEST: a different plugin digest is refused, naming the plugin\n");
    {
        NYA_NetContentManifest server = manifest_base(0x11);
        NYA_NetContentManifest client = manifest_base(0x12);

        NYA_NetClientState state = session_begin(&tick, &server, &client, false);
        nya_assert(state == NYA_NET_CLIENT_DISCONNECTED, "a client with a changed plugin was admitted");
        nya_assert_eq(nya_net_server_peer_count(), 0U);
        nya_assert(nya_net_client_disconnect_reason() == NYA_NET_DISCONNECT_CONTENT, "refused for reason %d", (int)nya_net_client_disconnect_reason());

        const NYA_NetContentMismatch* why = nya_net_client_content_mismatch();
        nya_assert(why != nullptr, "the refusal carried no detail");
        nya_assert(why->difference == NYA_NET_CONTENT_DIGEST && why->kind == NYA_NET_CONTENT_PLUGIN && nya_string_equals(why->id, "hello"));
        nya_assert(nya_string_equals(why->server_version, "0.1.0") && nya_string_equals(why->client_version, "0.1.0"));

        session_end();
    }

    printf("TEST: an extra plugin is refused, naming it\n");
    {
        NYA_NetContentManifest server = manifest_base(0x11);
        NYA_NetContentManifest client = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&client, entry_make(NYA_NET_CONTENT_PLUGIN, "aimbot", "6.6.6", 0x66, false)));

        NYA_NetClientState state = session_begin(&tick, &server, &client, true);
        nya_assert(state == NYA_NET_CLIENT_DISCONNECTED, "a client with an extra plugin was admitted");

        const NYA_NetContentMismatch* why = nya_net_client_content_mismatch();
        nya_assert(why != nullptr && why->difference == NYA_NET_CONTENT_EXTRA && nya_string_equals(why->id, "aimbot"));

        session_end();
    }

    printf("TEST: a client-only plugin is ignored by a server that allows them\n");
    {
        NYA_NetContentManifest server = manifest_base(0x11);
        NYA_NetContentManifest client = manifest_base(0x11);
        NYA_EXPECT(nya_net_content_add(&client, entry_make(NYA_NET_CONTENT_PLUGIN, "hud_skin", "2.0.0", 0x33, true)));

        NYA_NetClientState state = session_begin(&tick, &server, &client, true);
        nya_assert(state == NYA_NET_CLIENT_PLAYING, "a cosmetic plugin kept a client out");

        session_end();

        state = session_begin(&tick, &server, &client, false);
        nya_assert(state == NYA_NET_CLIENT_DISCONNECTED, "a strict server admitted a client-only plugin");
        nya_assert(nya_net_client_content_mismatch() != nullptr && nya_net_client_content_mismatch()->difference == NYA_NET_CONTENT_EXTRA);

        session_end();
    }

    printf("TEST: a command holding an action bit the game never registered is refused\n");
    {
        NYA_NetContentManifest content = manifest_base(0x11);

        nya_assert(session_begin(&tick, &content, &content, false) == NYA_NET_CLIENT_PLAYING);

        NYA_NetPeerId peer = nya_net_client_peer();

        HELD_ACTIONS = ACTION_UNKNOWN | 0x1ULL;

        u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
        while (nya_net_server_peer_stats(peer).violations == 0 && nya_clock_get_monotonic_ms() < deadline) {
            run_tick(&tick, true);
            sleep_ms(2);
        }

        nya_assert(nya_net_server_peer_stats(peer).violations > 0, "an unknown action bit was not counted");
        nya_assert((nya_net_server_last_command(peer).actions & ACTION_UNKNOWN) == 0, "a command with an unknown bit was applied");

        // a registered bit alone is applied as ever.
        HELD_ACTIONS = 0x1ULL;

        deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
        while (nya_net_server_last_command(peer).actions != 0x1ULL && nya_clock_get_monotonic_ms() < deadline) {
            run_tick(&tick, true);
            sleep_ms(2);
        }

        nya_assert_eq(nya_net_server_last_command(peer).actions, 0x1ULL);

        printf("  %u violations, and the next honest command applied\n", nya_net_server_peer_stats(peer).violations);

        HELD_ACTIONS = 0;
        session_end();
    }

    printf("TEST: an oversized or flooding game event is a violation, not a parse\n");
    {
        NYA_NetContentManifest content = manifest_base(0x11);

        nya_assert(session_begin(&tick, &content, &content, false) == NYA_NET_CLIENT_PLAYING);

        NYA_NetPeerId peer = nya_net_client_peer();

        // a document of one string past the bound.
        char* filler = nya_arena_alloc(arena, NYA_NET_EVENT_MAX_BYTES + 1);
        nya_memset(filler, 'x', NYA_NET_EVENT_MAX_BYTES);
        filler[NYA_NET_EVENT_MAX_BYTES] = '\0';

        NYA_Object* big = nya_object_create(arena);
        nya_object_add(big, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "padding" });
        nya_object_add(big, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = filler });
        NYA_EXPECT(nya_net_client_send_event(big));

        u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
        while (nya_net_server_peer_stats(peer).violations == 0 && nya_clock_get_monotonic_ms() < deadline) {
            run_tick(&tick, true);
            sleep_ms(2);
        }

        u32 after_big = nya_net_server_peer_stats(peer).violations;
        nya_assert_eq(after_big, 1U);

        // a burst's worth passes, and every one after it is counted.
        NYA_Object* small = nya_object_create(arena);
        nya_object_add(small, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "ping" });

        for (u32 i = 0; i < NYA_NET_EVENT_BURST + 8; i++) NYA_EXPECT(nya_net_client_send_event(small));

        deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
        while (nya_net_server_peer_stats(peer).violations < after_big + 4 && nya_clock_get_monotonic_ms() < deadline) {
            run_tick(&tick, true);
            sleep_ms(2);
        }

        nya_assert(nya_net_server_peer_stats(peer).violations >= after_big + 4, "a flood of events was not counted");

        printf("  one violation for the oversized event, %u in all after the flood\n", nya_net_server_peer_stats(peer).violations);

        session_end();
    }

    printf("PASSED: test_content (0 failures)\n");

    return EXIT_SUCCESS;
}
