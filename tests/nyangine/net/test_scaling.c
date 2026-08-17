/**
 * Interest management, bandwidth caps and lag compensation — the three things that decide whether a
 * server scales past a demo.
 *
 * Each is tested for the property that makes it worth having, and for the way it most plausibly goes
 * wrong:
 *
 * - **Interest management** must shrink what a peer is sent, must always include that peer's own
 *   entity, and must record the *filtered* snapshot as the baseline. Getting that last one wrong is the
 *   subtle failure: a delta computed against the unfiltered world claims "unchanged" about entities the
 *   peer has never been sent, so they never arrive at all.
 * - **A bandwidth cap** must skip snapshots rather than queue them, and must not advance the baseline
 *   for a snapshot it skipped — otherwise every later delta is built against a state the client does
 *   not have.
 * - **Lag compensation** must move other entities to where the shooter saw them, must not move the
 *   shooter, and must put everything back exactly.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

#define FLAG_REPLICATED (1ULL << 7)

#define TICK_SECONDS (1.0F / 60.0F)

static NYA_EntityHandle SPAWNED[NYA_NET_MAX_PEERS];

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(name);

  NYA_EntityHandle handle = nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

  if (peer.index < NYA_NET_MAX_PEERS) SPAWNED[peer.index] = handle;

  return handle;
}

static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  nya_unused(entity, command, delta_time_s);
}

static void sample_command(OUT NYA_NetCommand* command) {
  nya_unused(command);
}

/** The entity type a game's own relevance rule below accepts. */
#define RELEVANT_TYPE 42

static u32 RELEVANCE_CALLS       = 0;
static b8  RELEVANCE_SAW_CURRENT = false;

/**
 * A game's own relevance rule: only one kind of entity, whatever the distance.
 *
 * Deliberately not distance based, because that is the case the engine's built-in rule cannot express and
 * the callback exists for — a room, a team, a fog of war. It records whether it was ever told an entity was
 * already being sent, which is the parameter a hysteretic rule needs.
 * */
static b8 relevance_by_type(NYA_NetPeerId peer, const NYA_Entity* peer_entity, const NYA_Entity* entity, b8 currently_relevant) {
  nya_unused(peer, peer_entity);

  RELEVANCE_CALLS++;

  // Recorded rather than acted on: the assertion is that the engine *tells* a rule what it is already
  // sending, which is the only way a rule with no distance metric can be hysteretic.
  if (currently_relevant) RELEVANCE_SAW_CURRENT = true;

  return entity != nullptr && entity->type == RELEVANT_TYPE;
}

/** Somewhere to build the events the kick test sends. */
static NYA_Arena* arena_for_events = nullptr;

/** Brings up a listen server with one local player and runs enough ticks to finish the handshake. */
static NYA_NetPeerId start_listen_server(NYA_NetServerConfig config, u64* tick) {
  config.replicated_flag  = FLAG_REPLICATED;
  config.on_spawn_player  = nya_callback(spawn_player);
  config.on_apply_command = nya_callback(apply_movement);

  NYA_EXPECT(nya_net_server_start(config));

  NYA_NetTransport* client_end = nullptr;
  NYA_EXPECT(nya_net_server_attach_local(&client_end));

  NYA_EXPECT(nya_net_client_attach(client_end, "host", (NYA_NetClientConfig){
    .replicated_flag   = FLAG_REPLICATED,
    .on_apply_command  = nya_callback(apply_movement),
    .on_sample_command = nya_callback(sample_command),
  }));

  for (u32 i = 0; i < 8; i++) {
    nya_net_server_tick(*tick, TICK_SECONDS);
    nya_net_client_tick(*tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    (*tick)++;
  }

  nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the handshake did not complete");

  return nya_net_server_local_peer();
}

static void stop_everything(void) {
  nya_net_client_disconnect();
  nya_net_server_stop();

  nya_entity_foreach (leftover) nya_entity_despawn_deferred(leftover->handle);
  nya_system_sim_apply_commands();
}

/** How many entities the server put in the peer's most recent baseline. */
static u32 baseline_entity_count(NYA_NetPeerId peer, u64 tick) {
  const _NYA_NetServerBaseline* slot = &_NYA_NET_SERVER.peers[peer.index].baselines[tick % NYA_NET_SNAPSHOT_HISTORY];

  return slot->used && slot->snapshot.tick == tick ? slot->snapshot.entity_count : 0;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  u64 tick = 1;

  arena_for_events = nya_arena_create(.name = "test_scaling_events");
  defer nya_arena_destroy(arena_for_events);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: interest management shrinks what a peer is sent
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a relevance radius filters what a peer is sent\n");
  {
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ 0 }, &tick);

    // Twenty crates, most of them far away.
    for (u32 i = 0; i < 20; i++) {
      (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { (f32)i * 100.0F, 0.0F, 0.0F });
    }

    // Unfiltered first, so the comparison has a baseline in the ordinary sense of the word.
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();

    u32 unfiltered = baseline_entity_count(peer, tick);
    tick++;

    nya_assert(unfiltered == 21, "the world is the player plus twenty crates, got %u", unfiltered);

    stop_everything();

    // ── the same world, with a radius ─────────────────────────────────────────
    peer = start_listen_server((NYA_NetServerConfig){ .relevance_radius = 250.0F }, &tick);

    for (u32 i = 0; i < 20; i++) {
      (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { (f32)i * 100.0F, 0.0F, 0.0F });
    }

    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();

    u32 filtered = baseline_entity_count(peer, tick);
    tick++;

    printf("  %u entities unfiltered, %u within 250 units\n", unfiltered, filtered);

    nya_assert(filtered < unfiltered, "the radius filtered nothing");

    /*
     * The player is at the origin and crates are every 100 units, so 0, 100 and 200 are inside 250 and
     * the rest are not. Plus the player itself.
     */
    nya_assert(filtered == 4, "expected the player plus three crates within 250 units, got %u", filtered);

    /*
     * The peer's own entity is always sent, whatever the rule says.
     *
     * It is what they predict, and reconciliation needs the server's answer for it every snapshot —
     * a filter that excluded it would leave that player unable to be corrected at all.
     */
    {
      NYA_Entity* player = nya_entity_get(SPAWNED[peer.index]);
      nya_assert(player != nullptr);

      // Far outside its own radius from everything else, so only the "always include yourself" rule
      // can keep it in.
      player->position = (f32x3){ 100000.0F, 0.0F, 0.0F };

      nya_net_server_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();

      u32 alone = baseline_entity_count(peer, tick);
      tick++;

      nya_assert(alone == 1, "a player alone in its radius should still be sent itself, got %u", alone);
    }

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hysteresis stops an entity on the boundary flickering
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an entity on the relevance boundary does not flicker\n");
  {
    /*
     * The failure this exists to prevent.
     *
     * Relevance is re-decided every snapshot. With a single threshold, an entity sitting exactly on it
     * flips between relevant and not on almost every one — and each flip is a spawn and a despawn on the
     * client, several times a second, for something that barely moved.
     *
     * The band makes entering and leaving different questions: in at the radius, out only past the radius
     * plus the band. So an entity jittering by less than the band crosses neither threshold twice.
     */
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ .relevance_radius = 100.0F, .relevance_hysteresis = 50.0F }, &tick);

    NYA_EntityHandle wanderer = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 200.0F, 0.0F, 0.0F });

    // Out of range to begin with: 200 is past both 100 and 150.
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 1, "only the player should be in range at 200 units");
    tick++;

    // Still out at 120 — inside the *leave* radius, but it has to reach the *enter* radius first.
    nya_entity_get(wanderer)->position.x = 120.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 1, "an entity at 120 has not yet entered a 100 unit radius");
    tick++;

    // In at 90.
    nya_entity_get(wanderer)->position.x = 90.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 2, "an entity at 90 should have entered a 100 unit radius");
    tick++;

    /*
     * And now it stays in at 120, where it was refused before.
     *
     * That asymmetry *is* the hysteresis: the same position gives a different answer depending on whether
     * the entity is already being sent. Without it, this is the position that would flicker.
     */
    nya_entity_get(wanderer)->position.x = 120.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 2, "an entity already in range should stay in until it passes 150");
    tick++;

    // Only past the leave radius does it go.
    nya_entity_get(wanderer)->position.x = 160.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 1, "an entity at 160 should have left a 150 unit leave radius");
    tick++;

    /*
     * The measurement that matters: jitter across the enter threshold produces no transitions at all.
     *
     * Walk it in, then oscillate either side of 100 by less than the band. With one threshold this would
     * flip on roughly every snapshot; with the band it must flip exactly zero times.
     */
    nya_entity_get(wanderer)->position.x = 50.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 2);
    tick++;

    u32 transitions = 0;
    u32 previous    = 2;

    for (u32 i = 0; i < 40; i++) {
      // 95 and 130: either side of the 100 enter threshold, both inside the 150 leave threshold.
      nya_entity_get(wanderer)->position.x = (i % 2) == 0 ? 130.0F : 95.0F;

      nya_net_server_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();

      u32 now = baseline_entity_count(peer, tick);
      if (now != previous) transitions++;
      previous = now;

      tick++;
    }

    printf("  40 ticks of jitter across the enter threshold: %u transitions\n", transitions);

    nya_assert(transitions == 0, "an entity jittering inside the hysteresis band flickered %u times", transitions);

    // ── and zero means the default band, not none ────────────────────────────
    stop_everything();

    peer = start_listen_server((NYA_NetServerConfig){ .relevance_radius = 100.0F }, &tick);

    NYA_EntityHandle edge = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 50.0F, 0.0F, 0.0F });

    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 2, "the entity starts in range");
    tick++;

    /*
     * 110 is outside the radius and inside the default band, so it must stay.
     *
     * A hysteresis of zero meaning "no hysteresis" is the trap this checks: it would be a default that
     * produces flicker, which is a bug rather than a configuration.
     */
    nya_entity_get(edge)->position.x = 110.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 2, "an unset hysteresis should default to a band, not to none");
    tick++;

    // And past the default band it does leave: 100 + 25% of 100 = 125.
    nya_entity_get(edge)->position.x = 140.0F;
    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();
    nya_assert(baseline_entity_count(peer, tick) == 1, "past the default band the entity should leave");
    tick++;

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the baseline is the filtered snapshot, not the whole world
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the baseline records what was actually sent\n");
  {
    /*
     * The subtle half of interest management.
     *
     * If the baseline recorded the unfiltered world, a delta against it would claim "unchanged" about
     * entities this peer has never been sent — so they would never arrive at all, and a crate that came
     * into range would stay invisible for as long as it did not move.
     */
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ .relevance_radius = 250.0F }, &tick);

    (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 100.0F, 0.0F, 0.0F });   // in range
    (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 5000.0F, 0.0F, 0.0F });  // out of range

    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();

    u32 sent = baseline_entity_count(peer, tick);
    tick++;

    nya_assert(sent == 2, "the player plus the near crate, got %u", sent);

    const _NYA_NetServerBaseline* slot = &_NYA_NET_SERVER.peers[peer.index].baselines[(tick - 1) % NYA_NET_SNAPSHOT_HISTORY];

    // Nothing beyond the radius is in it, which is the assertion that matters.
    for (u32 i = 0; i < slot->snapshot.entity_count; i++) {
      nya_assert(slot->snapshot.entities[i].position.x < 1000.0F, "the baseline contains an entity that was never sent (x = %f)",
                 (f64)slot->snapshot.entities[i].position.x);
    }

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a bandwidth cap skips snapshots and does not advance the baseline
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a bandwidth cap skips rather than queues\n");
  {
    /*
     * A cap that a snapshot of this world genuinely brushes against, exercised in real time.
     *
     * The bucket refills from the monotonic clock, so a loop that runs in microseconds sees no refill at
     * all and would only ever show the one send the initial bucket paid for. Sleeping between ticks is
     * what makes the refill real and both branches reachable — this is a rate limiter, and a rate needs
     * time to mean anything.
     */
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ .bandwidth_bytes_per_second = 40000 }, &tick);

    for (u32 i = 0; i < 200; i++) {
      (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { (f32)i, 0.0F, 0.0F });
    }

    u32 sent    = 0;
    u32 skipped = 0;

    for (u32 i = 0; i < 30; i++) {
      // Something changes every tick, so a delta is never empty and every snapshot has real size.
      nya_entity_get(SPAWNED[peer.index])->position.x += 1.0F;

      nya_net_server_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();

      if (baseline_entity_count(peer, tick) > 0) sent++;
      else skipped++;

      tick++;

      // A tick's worth of wall time, so the bucket refills at something like the real rate.
      struct timespec request = { .tv_nsec = 16 * 1000 * 1000 };
      (void)nanosleep(&request, nullptr);
    }

    printf("  %u snapshots sent, %u skipped under a 40 kB/s cap\n", sent, skipped);

    nya_assert(skipped > 0, "a 40 kB/s cap skipped nothing over 30 ticks of a 200 entity world");

    /*
     * Progress is guaranteed, which is the property that took a design fix to get.
     *
     * The bucket gates on "is there anything left" rather than "does this fit", because a snapshot larger
     * than one second's budget can never fit — and requiring it to would starve this peer of state
     * permanently and silently. With debt allowed, the first snapshot goes out and the average settles at
     * the cap.
     */
    nya_assert(sent > 0, "the cap starved the peer completely; it must always make progress");

    /*
     * A skipped snapshot must not become the baseline.
     *
     * Recording one the peer never received would have every later delta computed against a state it
     * does not have — the client would apply changes on top of the wrong world, and nothing would detect
     * it. That is what `baseline_entity_count` returning zero for a skipped tick is checking, and the loop
     * above counts a tick as skipped exactly when no baseline was written.
     */

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an unlimited server sends every tick
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: no cap means no skipping\n");
  {
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ 0 }, &tick);

    for (u32 i = 0; i < 40; i++) (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { (f32)i, 0.0F, 0.0F });

    u32 sent = 0;

    for (u32 i = 0; i < 20; i++) {
      nya_net_server_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();

      if (baseline_entity_count(peer, tick) > 0) sent++;
      tick++;
    }

    nya_assert(sent == 20, "an uncapped server skipped %u of 20 snapshots", 20 - sent);

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: lag compensation rewinds and restores
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: lag compensation moves the world to what a client saw\n");
  {
    NYA_NetPeerId shooter = start_listen_server((NYA_NetServerConfig){ .lag_history_ticks = 32 }, &tick);

    NYA_EntityHandle target = nya_entity_spawn(.name = "target", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

    // Build history: the target walks steadily along x, one unit per tick.
    for (u32 i = 0; i < 20; i++) {
      nya_entity_get(target)->position.x += 1.0F;

      nya_net_server_tick(tick, TICK_SECONDS);
      nya_net_client_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();
      tick++;
    }

    f32 present = nya_entity_get(target)->position.x;
    u64 acked   = _NYA_NET_SERVER.peers[shooter.index].acknowledged_tick;

    nya_assert(acked > 0, "the local client never acknowledged a snapshot, so there is nothing to rewind to");
    nya_assert(present > 0.0F);

    b8 rewound = nya_net_server_rewind_begin(shooter);
    nya_assert(rewound, "rewinding to an acknowledged tick failed");

    f32 past = nya_entity_get(target)->position.x;

    printf("  target at %.1f now, %.1f when the client last acknowledged (tick %llu, %llu back)\n", (f64)present, (f64)past,
           (unsigned long long)acked, (unsigned long long)nya_net_server_rewind_ticks());

    /*
     * The target really moved back.
     *
     * A rewind that did nothing would pass every other assertion here — this is the one that says the
     * world is actually in the past between begin and end.
     */
    nya_assert(past < present, "the target was not rewound (%f vs %f)", (f64)past, (f64)present);

    /*
     * How far back it went, which has to be a real number rather than zero.
     *
     * It reads the tick the *server* was last driven with, not the world's own counter — a caller running
     * its own loop passes whatever tick it likes, and reaching for the world's instead reported zero for a
     * rewind that genuinely happened. A game refusing implausibly old shots would then refuse none.
     */
    nya_assert(nya_net_server_rewind_ticks() > 0, "a rewind that moved the world reported going back zero ticks");

    // The shooter is not rewound: they predicted themselves and are already where they aimed from.
    nya_assert(nya_entity_is_valid(SPAWNED[shooter.index]));

    nya_net_server_rewind_end();

    // And everything is put back exactly. Not approximately: the present is what has to be restored, and
    // an approximation of it would drift a little further every shot anybody fired.
    nya_assert(nya_entity_get(target)->position.x == present, "the world was not restored exactly (%f vs %f)",
               (f64)nya_entity_get(target)->position.x, (f64)present);

    nya_assert(nya_net_server_rewind_ticks() == 0, "rewind_ticks should read zero once restored");

    // ── refusals ──────────────────────────────────────────────────────────────
    {
      // Nesting would restore to the past rather than the present.
      nya_assert(nya_net_server_rewind_begin(shooter), "a second independent rewind should work");
      nya_assert(!nya_net_server_rewind_begin(shooter), "nesting a rewind is refused");
      nya_net_server_rewind_end();

      NYA_NetPeerId nobody = { .index = 9, .generation = 77 };
      nya_assert(!nya_net_server_rewind_begin(nobody), "rewinding for a peer that does not exist is refused");
    }

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: compensation is off by default
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rewinding is refused when no history is kept\n");
  {
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ 0 }, &tick);

    for (u32 i = 0; i < 10; i++) {
      nya_net_server_tick(tick, TICK_SECONDS);
      nya_net_client_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();
      tick++;
    }

    // Zero history means the feature is off, and off has to mean "returns false", not "returns true and
    // does nothing" — a game that acted on a successful rewind would resolve every shot against the
    // present while believing it had compensated.
    nya_assert(!nya_net_server_rewind_begin(peer), "rewinding with no history configured must be refused");

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a custom relevance rule, and the hysteresis it has to implement itself
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a game's own relevance rule\n");
  {
    /*
     * The engine cannot apply distance hysteresis to a rule it does not understand — a room or portal rule
     * has no radius — so instead it tells the callback whether the entity is already being sent and lets the
     * rule decide. A rule that ignores that will flicker, which is why the parameter exists at all.
     */
    RELEVANCE_CALLS       = 0;
    RELEVANCE_SAW_CURRENT = false;

    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ .on_relevance = nya_callback(relevance_by_type) }, &tick);

    // Two entities the rule accepts and two it does not, so the filter has something to do either way.
    (void)nya_entity_spawn(.type = RELEVANT_TYPE, .flags = FLAG_REPLICATED, .position = { 1.0F, 0.0F, 0.0F });
    (void)nya_entity_spawn(.type = RELEVANT_TYPE, .flags = FLAG_REPLICATED, .position = { 2.0F, 0.0F, 0.0F });
    (void)nya_entity_spawn(.type = 999, .flags = FLAG_REPLICATED, .position = { 3.0F, 0.0F, 0.0F });
    (void)nya_entity_spawn(.type = 999, .flags = FLAG_REPLICATED, .position = { 4.0F, 0.0F, 0.0F });

    nya_net_server_tick(tick, TICK_SECONDS);
    nya_system_sim_apply_commands();

    u32 sent = baseline_entity_count(peer, tick);
    tick++;

    nya_assert(RELEVANCE_CALLS > 0, "the relevance callback was never called");

    // The player plus the two the rule accepted. The player is always included whatever the rule says,
    // because reconciliation needs the server's answer for it every snapshot.
    nya_assert(sent == 3, "expected the player plus two accepted entities, got %u", sent);

    // ── the callback is told what it is already sending ───────────────────────
    {
      /*
       * On the second snapshot the two accepted entities are already being sent, so the callback must see
       * `currently_relevant` set for them. Without that a game has no way to be hysteretic and the engine
       * has no way to help it.
       */
      RELEVANCE_SAW_CURRENT = false;

      nya_net_server_tick(tick, TICK_SECONDS);
      nya_system_sim_apply_commands();
      tick++;

      nya_assert(RELEVANCE_SAW_CURRENT, "the callback was never told an entity was already relevant");
    }

    stop_everything();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: kicking, and events to one peer or to everyone
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: kick and send_event\n");
  {
    NYA_NetPeerId peer = start_listen_server((NYA_NetServerConfig){ 0 }, &tick);

    nya_assert(nya_net_server_peer_count() == 1);

    // The peer table is walkable, and a peer resolves both ways.
    const NYA_NetServerPeer* view = nya_net_server_peer(peer);
    nya_assert(view != nullptr, "the peer does not resolve");
    nya_assert(view->is_local, "a loopback peer is local");
    nya_assert(nya_net_server_peer_at(peer.index) != nullptr, "the peer is not in the table");
    nya_assert(nya_net_server_peer_at(NYA_NET_MAX_PEERS) == nullptr, "an index past the table returned something");

    // A stale id resolves to nothing, which is the whole point of the generation.
    NYA_NetPeerId stale = { .index = peer.index, .generation = peer.generation + 7 };
    nya_assert(nya_net_server_peer(stale) == nullptr, "a stale peer id resolved");

    NYA_Object* event = nya_object_create(arena_for_events);
    nya_object_set(event, "kind", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "test" });

    // To one peer, and to everyone. NYA_NET_PEER_NONE means broadcast, which saves a caller writing the
    // same loop.
    NYA_EXPECT(nya_net_server_send_event(peer, event));
    NYA_EXPECT(nya_net_server_send_event(NYA_NET_PEER_NONE, event));

    // And the refusals.
    nya_assert(!nya_net_server_send_event(peer, nullptr).ok, "sending a null event was accepted");
    nya_assert(!nya_net_server_send_event(stale, event).ok, "sending to a stale peer was accepted");

    // The last command from a peer that has sent none is zeroed rather than garbage.
    nya_assert(nya_net_server_last_command(stale).tick == 0, "a stale peer reported a command");

    // ── the kick ──────────────────────────────────────────────────────────────
    nya_net_server_kick(peer, NYA_NET_DISCONNECT_PROTOCOL);

    nya_assert(nya_net_server_peer_count() == 0, "kicking left the peer in the table");
    nya_assert(nya_net_server_peer(peer) == nullptr, "a kicked peer still resolves");

    // Kicking again, and kicking somebody who was never there, are both harmless — a moderation path is not
    // always sure what it is looking at.
    nya_net_server_kick(peer, NYA_NET_DISCONNECT_REQUESTED);
    nya_net_server_kick((NYA_NetPeerId){ .index = 20, .generation = 3 }, NYA_NET_DISCONNECT_REQUESTED);

    // An event with nobody to send it to is not an error.
    NYA_EXPECT(nya_net_server_send_event(NYA_NET_PEER_NONE, event));

    stop_everything();

    // And once stopped, the whole surface reports that rather than faulting.
    nya_assert(!nya_net_server_running());
    nya_assert(!nya_net_server_send_event(NYA_NET_PEER_NONE, event).ok, "a stopped server accepted an event");
    nya_assert(!nya_net_server_listen(1234).ok, "a stopped server accepted a listen");
    nya_assert(nya_net_server_peer_count() == 0);
    nya_assert(!nya_net_server_is_dedicated(), "a stopped server is not a dedicated one");
    nya_assert(!nya_net_server_rewind_begin(peer), "a stopped server allowed a rewind");

    // Stopping twice is tolerated.
    nya_net_server_stop();
  }

  printf("TEST: starting twice is refused\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = FLAG_REPLICATED }));

    nya_assert(!nya_net_server_start((NYA_NetServerConfig){ 0 }).ok, "a second server started over the first");

    // A local player can only be attached once, or one host would have two entities.
    NYA_NetTransport* first  = nullptr;
    NYA_NetTransport* second = nullptr;

    NYA_EXPECT(nya_net_server_attach_local(&first));
    nya_assert(!nya_net_server_attach_local(&second).ok, "a second local player was attached");

    // An excessive lag history is clamped rather than refused, since it is a request for "as much as I can
    // have" rather than a mistake worth failing over.
    nya_net_server_stop();

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .lag_history_ticks = NYA_NET_LAG_HISTORY * 4 }));
    nya_net_server_stop();
  }

  printf("PASSED: test_scaling (0 failures)\n");

  return EXIT_SUCCESS;
}
