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

/** A mark a landing left on the ground. See GNY_CUBE3D_MARK_COUNT. */
typedef struct GNY_Cube3DMark {
    f32x3     position;
    f32       rotation;
    f32       size;
    NYA_Color color;
    u8        cell;

    /** Uptime the mark was made, zero for a slot never used, and how long it lasts. */
    f32 born_s;
    f32 lifetime_s;
} GNY_Cube3DMark;

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

    /**
     * The simulated column over the same bonfire: a Navier-Stokes volume whose buoyancy carries the
     * heat the flames put into it. The billboards are the flames, this is the air above them.
     * */
    NYA_Fluid* plume;

    /** Seconds since the plume last emitted. */
    f32 plume_timer_s;

    /** The plume's crackle, looping. */
    NYA_SoundVoice fire_voice;

    /** The skinned bar's clock, and which of its clips it is on. */
    NYA_SkeletonAnimator bender;
    u32                  bender_clip;

    /** Stops the bar's clock where it is. */
    b8 bender_frozen;

    /** Zero until the model loads. */
    u32 bender_bone_count;

    /** A ring: the next landing writes `mark_next`. */
    GNY_Cube3DMark marks[GNY_CUBE3D_MARK_COUNT];
    u32            mark_next;

    /** Where the camera was last frame and when, for the speed lines. */
    f32x3 camera_previous;
    f32   camera_previous_s;

    /** The render feature switchboard is up. See layer_cube3d_features.c. */
    b8 features_open;
} GNY_Cube3DScene;

/**
 * Draws the render feature switchboard over the scene: one row per NYA_RenderFeature, writing
 * NYA_CONFIG.engine.renderer.features. Only while `features_open`; `0` toggles it.
 * */
void gny_layer_cube3d_features_draw(NYA_UI* ui, NYA_Window* window);
