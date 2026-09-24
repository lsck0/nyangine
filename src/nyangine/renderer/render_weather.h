/**
 * @file render_weather.h
 *
 * Rain and snow, over a region, as particles. No new draw path and no new shader: weather is a thin
 * skin over the particle system (render_particles.h), carried by the same analytic wind field
 * (render_wind.h) that already moves the foliage, the water and the drifting dust. A mode picks the
 * particle parameters and how hard the wind pulls, and the drops or flakes are emitted inside a box
 * that follows the camera, so the sky is full wherever you look without a world-sized particle count.
 *
 * ```c
 * NYA_Weather* weather = nya_weather_create(world->allocator, 4096);
 * nya_weather_wind_set(weather, &scene_wind);          // share the scene's one wind field
 * nya_weather_set(weather, NYA_WEATHER_RAIN, 0.7F);    // opt in; CLEAR is the default and emits nothing
 *
 * nya_weather_follow(weather, camera_target);          // once a frame, before the update
 * nya_weather_update(weather, delta_time_s);           // emits inside the moving box and integrates
 * nya_weather_draw(window, weather);                   // from a 3D layer's on_render, between begin/end
 * ```
 *
 * The mode-to-parameters mapping (nya_weather_params) and the box-follow origin (nya_weather_emit_center)
 * are pure functions, so a headless test pins rain-vs-snow and the follow behaviour without a GPU.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/renderer/render_particles.h"
#include "nyangine/renderer/render_wind.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_WeatherMode     NYA_WeatherMode;
typedef struct NYA_WeatherParams NYA_WeatherParams;
typedef struct NYA_Weather       NYA_Weather;

/** What is falling. CLEAR is the default: nothing emits, so a scene is dry until it opts in. */
enum NYA_WeatherMode {
    /** Nothing falls. Emission rate is zero, so the pool empties out and the draw is a no-op. */
    NYA_WEATHER_CLEAR = 0,

    /** Fast, near-vertical, only slightly leaned by the wind: a downpour. */
    NYA_WEATHER_RAIN,

    /** Slow, fluttery and strongly wind-drifted: flakes that hang and wander. */
    NYA_WEATHER_SNOW,

    NYA_WEATHER_COUNT,
};

/**
 * The particle behaviour a mode and intensity resolve to. A pure product of nya_weather_params, kept
 * separate from the live system so the mapping can be checked on its own. Fed straight into a
 * NYA_ParticleBurst and nya_particles_wind_set when the mode is applied.
 * */
struct NYA_WeatherParams {
    /** Which way a drop or flake initially heads, before gravity and the wind bend it. Down, with a lean. */
    f32x3 direction;

    /** World units per second, sampled per particle. */
    f32x2 speed;

    /** Seconds a drop or flake lives. Short for rain, long for the slow drift of snow. */
    f32x2 lifetime_s;

    /** World units. Small hard drops, larger soft flakes. */
    f32x2 size;
    f32x2 size_end;

    NYA_Color color_start;
    NYA_Color color_end;

    /** World units per second squared, pulling straight down. Strong for rain, gentle for snow. */
    f32x3 gravity;

    /**
     * How hard the shared wind field pulls a particle's velocity toward it, passed to
     * nya_particles_wind_set. Low for rain (a slight lean), high for snow (it wanders on the air).
     * */
    f32 wind_influence;

    /** Particles released per second across the whole emission box, already scaled by intensity. */
    f32 emit_per_second;
};

/**
 * A weather system: a bounded particle pool, a borrowed wind field, and a box that follows a target so
 * the emission tracks the view. The caller owns the arena and the wind field.
 * */
struct NYA_Weather {
    /** The pool weather draws through. Its capacity is the hard ceiling on live drops/flakes. */
    NYA_ParticleSystem* particles;

    /** The shared wind field, borrowed. Null until nya_weather_wind_set; weather still falls without it. */
    const NYA_WindField* wind;

    NYA_WeatherMode mode;

    /** In [0, 1]. Scales the emission rate between none and the mode's full downpour/flurry. */
    f32 intensity;

    /** Full extents of the emission box: how wide (x), how tall (y) and how deep (z) the sky slab is. */
    f32x3 box;

    /** How far above the follow target the box floats, so drops start overhead and fall past the camera. */
    f32 ceiling;

    /** The last target handed to nya_weather_follow. The box centres on its xz. */
    f32x3 target;

    /** Carries the fractional part of emit_per_second * dt across frames, so the rate does not ride fps. */
    f32 emit_accumulator;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PURE MATH
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The particle behaviour for a mode at an intensity in [0, 1]. Pure: the same arguments always give the
 * same parameters, which is what the weather test rests on. CLEAR (and any out-of-range mode) yields a
 * zero emission rate, so nothing is spawned.
 * */
NYA_API NYA_WeatherParams nya_weather_params(NYA_WeatherMode mode, f32 intensity) __attr_no_discard;

/**
 * Where a burst of `box`-sized weather is centred so it sits `ceiling` above `target`: the target's xz,
 * raised to `target.y + ceiling`. Pure, so the box-follows-camera behaviour is checkable without a frame.
 * */
NYA_API f32x3 nya_weather_emit_center(f32x3 target, f32x3 box, f32 ceiling) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds a weather system with room for `capacity` drops or flakes at once, drawn as 3D billboards. The
 * pool caps the particle count, so weather can never grow without bound. Starts CLEAR.
 * */
NYA_API NYA_Weather* nya_weather_create(NYA_Arena* arena, u32 capacity) __attr_no_discard;

/**
 * Binds the wind field the drops and flakes drift on, borrowed from the caller. Null turns the drift off.
 * The influence is set per mode by nya_weather_set, so several systems and the foliage can share one wind.
 * */
NYA_API void nya_weather_wind_set(NYA_Weather* weather, const NYA_WindField* field);

/**
 * Switches the weather to `mode` at `intensity` in [0, 1], applying the mode's particle parameters and its
 * wind influence. CLEAR (the default) stops new emission; the pool then empties as its particles age out.
 * */
NYA_API void nya_weather_set(NYA_Weather* weather, NYA_WeatherMode mode, f32 intensity);

/** Points the emission box at a target, usually the camera or what it looks at. Call before the update. */
NYA_API void nya_weather_follow(NYA_Weather* weather, f32x3 target);

/** Sets the emission box extents and how far above the target it floats. Both have usable defaults. */
NYA_API void nya_weather_box_set(NYA_Weather* weather, f32x3 box, f32 ceiling);

/**
 * Emits the frame's drops or flakes inside the moving box and integrates every live one. A no-op amount
 * of emission when CLEAR, but the particles still integrate so a downpour tapers off rather than vanishing.
 * */
NYA_API void nya_weather_update(NYA_Weather* weather, f32 delta_time_s);

/** Draws the weather through the particle system's 3D billboard path. Call between begin and end. */
NYA_API void nya_weather_draw(NYA_Window* window, const NYA_Weather* weather);

/** The live drop/flake count. Zero once CLEAR has drained the pool. */
NYA_API u32 nya_weather_count(const NYA_Weather* weather) __attr_no_discard;

/** The current mode. */
NYA_API NYA_WeatherMode nya_weather_mode(const NYA_Weather* weather) __attr_no_discard;
