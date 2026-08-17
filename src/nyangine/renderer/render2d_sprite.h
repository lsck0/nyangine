/**
 * @file render2d_sprite.h
 *
 * Sprites and sprite atlases: a named region of a texture, drawn with a pivot, a flip and a turn.
 *
 * A thin layer over nya_render2d_texture_ex rather than a second drawing path — that call already takes
 * a source rectangle, a destination, an origin, a rotation, two flips and a tint, which is the whole
 * of what a sprite is. What it does not do is remember any of it, or work out which cell of a sheet
 * frame seven is. That is what these two types add.
 *
 * ```c
 * NYA_SpriteAtlas atlas  = nya_sprite_atlas_grid(NYA_ASSET_ART_HERO_PNG, 32, 32);
 * NYA_Sprite      sprite = nya_sprite_from_atlas(&atlas, 3);
 *
 * sprite.flip_x = facing_left;
 *
 * nya_render2d_sprite(window, &sprite, position);
 * ```
 *
 * ## Drawing something the solver owns
 *
 * A rigid body's transform is a centre and an angle, which is exactly what a sprite wants — so
 * drawing a simulated entity is a copy of two fields:
 *
 * ```c
 * sprite.rotation = nya_physics2d_rotation(entity);
 * nya_render2d_sprite(window, &sprite, (f32x2){ entity->position.x, entity->position.y });
 * ```
 *
 * Deliberately not a `nya_render2d_sprite_entity` that reads those itself. This file is the renderer and
 * knows nothing about entities or physics; a call that reached into either would invert the layering
 * for the sake of saving one line at the call site.
 *
 * ## Pivots are fractions
 *
 * `origin` is in units of the sprite's own size rather than pixels, so `{ 0.5, 0.5 }` is the centre
 * of any frame and `{ 0.5, 1 }` is the middle of its feet whatever the sheet's cell size is. Pixel
 * origins mean editing every sprite when the art is redrawn a little larger.
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
 *
 * Uniform on purpose. A packed atlas with per frame rectangles is what a build time packer produces
 * and wants a generated table to go with it; a grid is what hand drawn sheets are, needs four
 * numbers, and is the case that comes up first. The two are not exclusive — nya_sprite_from_rect
 * takes an arbitrary rectangle and skips this entirely.
 *
 * Plain data with no allocation and no handle: copy it, put it in a constant, hold it by value.
 * */
struct NYA_SpriteAtlas {
    /** The texture asset every frame is cut from. */
    NYA_ConstCString texture;

    /** Size of one cell, in the texture's pixels. */
    u32 frame_width;
    u32 frame_height;

    /**
     * Cells across and down.
     *
     * Derived by nya_sprite_atlas_grid from the loaded texture's size, so a sheet that is re-exported
     * with another row of frames needs no code change. Zero until the texture has finished loading,
     * which is why nya_sprite_atlas_frame_count can answer zero.
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
 *
 * Everything except the position, because a sprite is usually drawn at somewhere that changes every
 * frame while the rest of it does not. Zero initialising is meaningful — an all-zero `tint` is white
 * rather than invisible, and an all-zero `scale` is one — so
 * `(NYA_Sprite){ .texture = handle }` is a complete sprite.
 * */
struct NYA_Sprite {
    /** The texture. Set by the constructors; set it directly for a sprite that is a whole image. */
    NYA_ConstCString texture;

    /**
     * The part of the texture to draw, in its pixels. A zero width or height means the whole thing.
     *
     * Already resolved from the atlas and frame by the time it is here, so drawing does not need the
     * atlas to still be around — which is what lets an atlas be a local that goes out of scope.
     * */
    f32 source_x, source_y, source_width, source_height;

    /**
     * Where the pivot sits within the sprite, as a fraction of its size.
     *
     * `{ 0.5, 0.5 }` is the centre and is what a rotating thing almost always wants; `{ 0, 0 }` is
     * the top left, which is what every other draw call in the renderer positions by. Zero therefore
     * means the top left, matching nya_render2d_rect rather than nya_render2d_rect_rotated.
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
 * The other way art arrives: a folder of numbered PNGs, which is what most hand drawn animation and
 * most asset packs are before anyone runs a packer over them. Indexed exactly like an atlas, so
 * swapping between the two is changing which constructor a sprite came from.
 *
 * ## What it costs
 *
 * **A draw call per texture change.** One draw call has one texture, so consecutive sprites out of
 * one atlas batch into a single call while consecutive sprites out of a list cost one each — see the
 * batching note in render2d.h. For a handful of animated things that is nothing; for a thousand
 * particles it is the difference between one call and a thousand, and the answer there is an atlas.
 *
 * The array is borrowed, not copied. Point it at a static table of asset handles — which is what the
 * generated asset index gives you — and it outlives every sprite made from it.
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
 *
 * Capped by NYA_SPRITE_ANIMATION_MAX_SIGNALS, which is why a very slow frame with a very fast
 * animation drops some rather than growing a buffer — the alternative is a hitch turning into an
 * allocation.
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
     *
     * Fires once per entry into the frame, so a looping animation fires it once per loop, and a tick
     * long enough to skip past the frame entirely still fires it — a hit that only lands when the
     * frame rate is good is worse than no hit at all.
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
 *
 * Frames are indices into whichever atlas or list the animator is drawn from, so an animation does
 * not name a texture — which is what lets one walk cycle drive four differently coloured sheets.
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
     *
     * For a cycle whose ends meet — a torch flicker, an idle breath. Wrong for a walk, where the
     * last frame's foot is already where the first frame's foot goes. The reversed half emits frame
     * events again, because a marker means "this frame is showing" and it is.
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
     *
     * Positive only. Negative would have to run every loop, marker and finish rule backwards, which
     * is twice the logic for something no caller has wanted — a rewind is a ping-pong animation, or
     * a second animation authored in reverse.
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
 *
 * Restarting rather than resuming is what "play the attack" means every time it is said — an attack
 * interrupted and re-triggered starts over. Use nya_sprite_animator_resume to continue.
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
 * Returns how many signals were written, capped at `capacity`. Passing null and zero advances
 * without reporting, which is what a purely visual animation wants.
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
 *
 * The row and column counts come from the texture's own size, so this can be called before the asset
 * has loaded — it simply reports zero frames until it has, and recomputing is one call.
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
 *
 * `textures` is borrowed rather than copied, so it has to outlive the list.
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
 *
 * The frame is resolved now rather than remembered, so the atlas does not have to outlive the sprite.
 * Changing frame is another call — animation is a frame index and a timer, and neither belongs here.
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
 *
 * A missing or still loading texture draws nothing rather than failing — assets load asynchronously,
 * so the first frames after a load legitimately have nothing to show.
 * */
NYA_API void nya_render2d_sprite(NYA_Window* window, const NYA_Sprite* sprite, f32x2 position);
