/**
 * @file sim.h
 *
 * ```c
 * // in the hook: state a fact, decide nothing
 * nya_sim_record(GNY_SIM_IMPACT, &(GNY_SimImpact){ .point = hit->point.xy, ... }, sizeof(GNY_SimImpact));
 *
 * // in the observer: see all of them, then act once
 * ```
 * */
#pragma once

#include "nyangine-core/nyangine.h"

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
 * */
void gny_sim_observe(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data);
