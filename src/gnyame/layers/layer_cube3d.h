/**
 * @file layer_cube3d.h
 *
 * The 3D demo's state: an orbit camera, a draggable cube, two loaded models, a pile of cubes on a
 * noise terrain, and particle effects.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "gnyame/constants.h"

/** One cube in the pile. The pool is fixed; see GNY_TERRAIN3D_CUBE_COUNT. */
typedef struct GNY_FallingCube {
    NYA_EntityHandle entity;

    /** Full edge length in metres, kept for drawing. */
    f32       size;
    NYA_Color color;

    /** Translucent, drawn in the sorted transparent pass, frosted by `blur` in [0, 1]. */
    b8  glass;
    f32 blur;
} GNY_FallingCube;

typedef struct GNY_Cube3DScene {
    NYA_EntityHandle cube;
    NYA_EntityHandle model;
    NYA_EntityHandle pill;

    NYA_Terrain3D* terrain;

    GNY_FallingCube cubes[GNY_TERRAIN3D_CUBE_COUNT];
    u32             cube_count;

    /** Cubes that fell off the world and were put back. Shown in the HUD. */
    u32 cubes_recycled;

    /** Radians, with pitch clamped short of the poles. */
    f32 orbit_yaw;
    f32 orbit_pitch;

    /** Metres from the target. */
    f32 orbit_range;

    /** Left button went down on the cube, not merely down. */
    b8 dragging;

    /** The hint stops once the cube has been grabbed. */
    b8 grabbed_once;

    NYA_ParticleSystem* dust;

    /** Fire adds and smoke blends, so they are two systems. */
    NYA_ParticleSystem* fire;
    NYA_ParticleSystem* smoke;

    /** Seconds since the plume last emitted. */
    f32 plume_timer_s;
} GNY_Cube3DScene;
