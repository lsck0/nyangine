/**
 * @file render2d_sprite.h
 *
 * ```c
 * NYA_SpriteAtlas atlas  = nya_sprite_atlas_grid(NYA_ASSET_ART_HERO_PNG, 32, 32);
 * NYA_Sprite      sprite = nya_sprite_from_atlas(&atlas, 3);
 *
 * sprite.flip_x  = facing_left;
 * sprite.rotation = nya_physics2d_rotation(entity);   // a body's transform is a centre and an angle
 *
 * nya_render2d_sprite(window, &sprite, position);
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/renderer/render2d.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SpriteAtlas NYA_SpriteAtlas;
typedef struct NYA_SpriteList  NYA_SpriteList;
typedef struct NYA_Sprite      NYA_Sprite;

/**
 * A texture cut into a uniform grid of frames, numbered left to right and then top to bottom.
 * */
struct NYA_SpriteAtlas {
    /** The texture asset every frame is cut from. */
    NYA_ConstCString texture;

    /** Size of one cell, in the texture's pixels. */
    u32 frame_width;
    u32 frame_height;

    /**
     * Cells across and down.
     * */
    u32 columns;
    u32 rows;

    /** Transparent pixels between cells, if the sheet was exported with any. Usually zero. */
    u32 spacing;

    /** Transparent border around the whole sheet, if any. Usually zero. */
    u32 margin;
};

/**
 * One drawable thing: where in a texture it is, and how it should be put on screen.
 * */
struct NYA_Sprite {
    /** The texture. Set by the constructors; set it directly for a sprite that is a whole image. */
    NYA_ConstCString texture;

    /**
     * The part of the texture to draw, in its pixels. A zero width or height means the whole thing.
     * */
    f32 source_x, source_y, source_width, source_height;

    /**
     * Where the pivot sits within the sprite, as a fraction of its size.
     * */
    f32x2 origin;

    /** Multiplies the source size. Zero on either axis is read as one, so a zeroed sprite is 1:1. */
    f32x2 scale;

    /** Clockwise, in radians, about `origin`. The same sense as a rigid body's angle. */
    f32 rotation;

    b8 flip_x;
    b8 flip_y;

    /** Multiplies the texture. All-zero is read as white, i.e. untouched. */
    NYA_Color tint;
};

/**
 * Frames as separate images, one texture each, instead of cells of one sheet.
 *
 * ⚠ **A draw call per texture change.** Consecutive sprites out of one atlas batch into a single call;
 * out of a list they cost one each. Nothing for a handful of animated things, the difference between
 * one call and a thousand for particles — where the answer is an atlas.
 * */
struct NYA_SpriteList {
    const NYA_ConstCString* textures;
    u32                     count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ANIMATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * An animation is a span of frames and a rate; an animator is where in one you are. They are
 * separate because the first is *content* — shared, constant, usually a static table — and the
 * second is *state*, one per thing playing it. A hundred goblins share one walk cycle and each has
 * its own animator.
 *
 * ```c
 * static const NYA_SpriteAnimationEvent attack_events[] = {
 *     { .frame = 4, .id = ATTACK_CONNECTS },
 * };
 *
 * static const NYA_SpriteAnimation attack = {
 *     .first_frame = 12, .frame_count = 7, .frames_per_second = 14.0F,
 *     .events = attack_events, .event_count = 1,
 * };
 *
 * nya_sprite_animator_play(&entity_state->animator, &attack);
 * ```
 *
 * ## Why events and not just "the animation finished"
 *
 * Because the interesting moment is almost never the end. An attack starts its swing and its sound
 * immediately, and *lands* on the frame where the blade is out — which is frame four of seven, and
 * is a number that changes every time an artist retimes the animation. Reading it off the animation
 * means the retime moves the hit with it; hardcoding a timer in the game means it silently stops
 * matching.
 *
 * Signals come out of nya_sprite_animator_advance as a small array, and the entity system forwards
 * them to NYA_Entity.on_animation. Both are the same list; a game that does not use entities polls
 * it directly.
 */

typedef enum NYA_SpriteAnimationSignalKind NYA_SpriteAnimationSignalKind;
typedef struct NYA_SpriteAnimationEvent    NYA_SpriteAnimationEvent;
typedef struct NYA_SpriteAnimation         NYA_SpriteAnimation;
typedef struct NYA_SpriteAnimationSignal   NYA_SpriteAnimationSignal;
typedef struct NYA_SpriteAnimator          NYA_SpriteAnimator;

/**
 * Signals one advance can produce. A tick that crosses several frames produces several.
 * */
#ifndef NYA_SPRITE_ANIMATION_MAX_SIGNALS
#define NYA_SPRITE_ANIMATION_MAX_SIGNALS 16
#endif

/** Most frame markers one animation may carry. */
#ifndef NYA_SPRITE_ANIMATION_MAX_EVENTS
#define NYA_SPRITE_ANIMATION_MAX_EVENTS 16
#endif

enum NYA_SpriteAnimationSignalKind {
    /** The first advance after nya_sprite_animator_play. Fires before any frame signal. */
    NYA_SPRITE_ANIMATION_STARTED = 0,

    /**
     * A frame carrying an NYA_SpriteAnimationEvent was reached. `id` is the game's.
     * */
    NYA_SPRITE_ANIMATION_EVENT,

    /** A looping animation wrapped. Does not fire for the last loop of a non-looping one. */
    NYA_SPRITE_ANIMATION_LOOPED,

    /** A non-looping animation reached its last frame and stopped. Fires exactly once. */
    NYA_SPRITE_ANIMATION_FINISHED,

    NYA_SPRITE_ANIMATION_SIGNAL_KIND_COUNT,
};

/**
 * A marker on one frame of an animation. What "trigger the hit when the blade is out" is.
 *
 * `id` is game defined and the engine never interprets it, the same contract NYA_Entity.type has.
 * */
struct NYA_SpriteAnimationEvent {
    /** Index within the animation, not within the atlas. Frame zero is the animation's first. */
    u32 frame;

    u32 id;
};

/**
 * A span of frames and how fast to play them. Constant; share one between everything playing it.
 * */
struct NYA_SpriteAnimation {
    /** Where in the atlas the animation starts. */
    u32 first_frame;

    /** How many frames it runs for. Zero is an animation that never advances, not an error. */
    u32 frame_count;

    /** Zero is read as NYA_SPRITE_ANIMATION_DEFAULT_FPS, so a bare span still plays. */
    f32 frames_per_second;

    /** Wraps to the first frame and emits LOOPED. Otherwise it stops on the last and emits FINISHED. */
    b8 looping;

    /**
     * Plays forward then backward rather than snapping back to the start.
     * */
    b8 ping_pong;

    /** Borrowed, not copied. Point it at a static table. */
    const NYA_SpriteAnimationEvent* events;
    u32                             event_count;
};

/** What one advance produced. */
struct NYA_SpriteAnimationSignal {
    NYA_SpriteAnimationSignalKind kind;

    /** The animation-local frame it happened on. */
    u32 frame;

    /** The event's `id`, for NYA_SPRITE_ANIMATION_EVENT. Zero otherwise. */
    u32 id;
};

/** Frames per second for an animation that does not say. Twelve is the usual hand-drawn cadence. */
#ifndef NYA_SPRITE_ANIMATION_DEFAULT_FPS
#define NYA_SPRITE_ANIMATION_DEFAULT_FPS 12.0F
#endif

/**
 * Where in an animation something is. One per thing playing; the animation itself is shared.
 *
 * Plain data with no allocation, so it sits inside a game's own struct or on an entity by value.
 * */
struct NYA_SpriteAnimator {
    /** Borrowed. Null when nothing is playing. */
    const NYA_SpriteAnimation* animation;

    /** Seconds into the current frame, not into the animation. */
    f32 frame_elapsed_s;

    /** Index within the animation. Add `animation->first_frame` for the atlas index. */
    u32 frame;

    /**
     * Multiplies the rate. Zero pauses without clearing the animation.
     * */
    f32 speed;

    b8 playing;

    /** True once a non-looping animation has reached its end. Cleared by the next play. */
    b8 finished;

    /** Which way a ping-pong animation is currently going. Meaningless otherwise. */
    b8 reversing;

    /** Completed loops since the last play. For "swing three times then stop". */
    u32 loops;

    /** Set by play, consumed by the first advance, which is what makes STARTED fire exactly once. */
    b8 pending_start;
};

/**
 * Starts `animation` from its first frame. Restarts it if it is already playing.
 * */
NYA_API void nya_sprite_animator_play(OUT NYA_SpriteAnimator* animator, const NYA_SpriteAnimation* animation);

/** Stops without clearing the animation, so resume continues from here. */
NYA_API void nya_sprite_animator_pause(OUT NYA_SpriteAnimator* animator);
NYA_API void nya_sprite_animator_resume(OUT NYA_SpriteAnimator* animator);

/** Stops and forgets the animation. The animator draws nothing until something is played on it. */
NYA_API void nya_sprite_animator_stop(OUT NYA_SpriteAnimator* animator);

/**
 * Advances by `delta_time_s` and writes what happened into `out_signals`.
 *
 * ```c
 * NYA_SpriteAnimationSignal signals[NYA_SPRITE_ANIMATION_MAX_SIGNALS];
 * u32 count = nya_sprite_animator_advance(&animator, delta_time_s, signals, nya_carray_length(signals));
 *
 * for (u32 i = 0; i < count; i++) {
 *     if (signals[i].kind == NYA_SPRITE_ANIMATION_EVENT && signals[i].id == ATTACK_CONNECTS) strike();
 * }
 * ```
 * */
NYA_API u32 nya_sprite_animator_advance(OUT NYA_SpriteAnimator* animator, f32 delta_time_s, OUT NYA_SpriteAnimationSignal* out_signals, u32 capacity);

/** The atlas frame the animator is showing. Zero when nothing is playing. */
NYA_API u32 nya_sprite_animator_frame(const NYA_SpriteAnimator* animator) __attr_no_discard;

/** Points `sprite` at the animator's current frame of `atlas`. What a draw call needs. */
NYA_API void nya_sprite_animator_apply(const NYA_SpriteAnimator* animator, const NYA_SpriteAtlas* atlas, OUT NYA_Sprite* sprite);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ATLASES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Describes a texture as a grid of `frame_width` by `frame_height` cells.
 * */
NYA_API NYA_SpriteAtlas nya_sprite_atlas_grid(NYA_ConstCString texture, u32 frame_width, u32 frame_height) __attr_no_discard;

/** Same, for a sheet exported with padding between cells or a border around them. */
NYA_API NYA_SpriteAtlas nya_sprite_atlas_grid_padded(NYA_ConstCString texture, u32 frame_width, u32 frame_height, u32 spacing, u32 margin)
    __attr_no_discard;

/** Frames the grid holds. Zero while the texture is still loading, since its size is not known yet. */
NYA_API u32 nya_sprite_atlas_frame_count(const NYA_SpriteAtlas* atlas) __attr_no_discard;

/** Where a frame sits in the texture, in its pixels. A zeroed rectangle for an out of range index. */
NYA_API void nya_sprite_atlas_frame_rect(const NYA_SpriteAtlas* atlas, u32 frame, OUT f32* out_x, OUT f32* out_y, OUT f32* out_width,
                                         OUT f32* out_height);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * IMAGE LISTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Describes an ordered set of separate images as frames.
 *
 * ```c
 * static NYA_ConstCString walk[] = { NYA_ASSET_ART_WALK_0_PNG, NYA_ASSET_ART_WALK_1_PNG };
 *
 * NYA_SpriteList frames = nya_sprite_list(walk, 2);
 * NYA_Sprite     sprite = nya_sprite_from_list(&frames, 0);
 * ```
 * */
NYA_API NYA_SpriteList nya_sprite_list(const NYA_ConstCString* textures, u32 count) __attr_no_discard;

/** Frames the list holds. Unlike an atlas this is known immediately — no texture has to have loaded. */
NYA_API u32 nya_sprite_list_frame_count(const NYA_SpriteList* list) __attr_no_discard;

/** A sprite showing one image of a list, centred, unflipped and untinted. */
NYA_API NYA_Sprite nya_sprite_from_list(const NYA_SpriteList* list, u32 frame) __attr_no_discard;

/** Points an existing sprite at another image, keeping its flip, tint, scale and rotation. */
NYA_API void nya_sprite_set_frame_from_list(OUT NYA_Sprite* sprite, const NYA_SpriteList* list, u32 frame);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SPRITES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A sprite showing one frame of an atlas, centred, unflipped and untinted.
 * */
NYA_API NYA_Sprite nya_sprite_from_atlas(const NYA_SpriteAtlas* atlas, u32 frame) __attr_no_discard;

/** A sprite showing an arbitrary rectangle of a texture. The escape hatch from the uniform grid. */
NYA_API NYA_Sprite nya_sprite_from_rect(NYA_ConstCString texture, f32 x, f32 y, f32 width, f32 height) __attr_no_discard;

/** Points an existing sprite at another frame, keeping its flip, tint, scale and rotation. */
NYA_API void nya_sprite_set_frame(OUT NYA_Sprite* sprite, const NYA_SpriteAtlas* atlas, u32 frame);

/** What the sprite covers on screen once scale is applied, in world or screen units. */
NYA_API f32x2 nya_sprite_size(const NYA_Sprite* sprite) __attr_no_discard;

/**
 * Draws the sprite with its pivot at `position`.
 * */
NYA_API void nya_render2d_sprite(NYA_Window* window, const NYA_Sprite* sprite, f32x2 position);
