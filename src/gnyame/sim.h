/**
 * @file sim.h
 *
 * What happened this frame, and what the game does about it once the frame is over.
 *
 * The third of the three ways code is written here, after hooks on entities and systems that query
 * for a kind or a flag. A hook decides *that* something happened; it does not decide what the
 * consequences are. It records a fact with nya_sim_record and returns, and an observer reads the
 * whole frame's worth at the barrier and acts on all of it at once.
 *
 * ## Why that is better than acting in the hook
 *
 * The impact sound is the worked example. Acting in the hook meant the first six crates to land in a
 * tick got a voice and the seventh — which might have been the one that fell from the top of the
 * screen — got nothing, because a hook can only see the impact it is holding. The observer sees
 * every impact in the frame at once and can spend its six voices on the six *loudest*, which is the
 * decision anyone would actually want.
 *
 * It also removed per-tick state the hook had to keep in the world just to know when to reset a
 * counter. That is the usual shape of this: work that looks like it needs bookkeeping in the hook is
 * usually work that belongs at the barrier.
 *
 * ```c
 * // in the hook: state a fact, decide nothing
 * nya_sim_record(GNY_SIM_IMPACT, &(GNY_SimImpact){ .point = hit->point.xy, ... }, sizeof(GNY_SimImpact));
 *
 * // in the observer: see all of them, then act once
 * ```
 *
 * Records are cleared at the end of every frame, so an observer sees exactly the frame it is being
 * told about. Anything that has to outlive the frame is the observer's job to copy somewhere — which
 * for this game means the counters on GNY_World.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RECORD TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum GNY_SimRecordType GNY_SimRecordType;
typedef struct GNY_SimImpact   GNY_SimImpact;
typedef struct GNY_SimBoxLost  GNY_SimBoxLost;

/** What NYA_SimRecord.type holds here. Game defined; the engine never interprets it. */
enum GNY_SimRecordType {
    GNY_SIM_NONE = 0,

    /** Two bodies met hard enough to be worth hearing. Recorded by the crate's on_collision. */
    GNY_SIM_IMPACT,

    /** A crate fell out of the world and was despawned. Recorded by the crate's on_update. */
    GNY_SIM_BOX_LOST,
};

struct GNY_SimImpact {
    NYA_EntityHandle a;
    NYA_EntityHandle b;

    /** World units. Where the sound is placed from. */
    f32x2 point;

    /** World units per second, and what the observer ranks impacts by. */
    f32 approach_speed;
};

struct GNY_SimBoxLost {
    NYA_EntityHandle box;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registers the observer. Called once from gnyame_init, after the world exists. */
void gny_sim_init(void);

/**
 * Reads the frame's records and acts on all of them at once.
 *
 * Registered with nya_callback, so it is resolved by name and survives a hot reload — which is why
 * it is exported rather than NYA_INTERNAL. An observer stored as a raw pointer into this library
 * would aim at an unmapped page the first time the game was rebuilt.
 * */
void gny_sim_observe(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data);
