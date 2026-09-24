#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The texture's pixel size, or false while it is missing or still loading. */
NYA_INTERNAL b8 _nya_sprite_texture_size(NYA_ConstCString texture, OUT u32* out_width, OUT u32* out_height);

/**
 * The atlas grid, from the texture if the atlas was described before it loaded. A grid built at startup
 * would otherwise stay empty forever and every frame would draw the whole sheet.
 * */
NYA_INTERNAL void _nya_sprite_atlas_grid_size(const NYA_SpriteAtlas* atlas, OUT u32* out_columns, OUT u32* out_rows);

// Compiled in both the real and the headless build, unlike render_draw.

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ATLASES
 * ─────────────────────────────────────────────────────────
 */

NYA_SpriteAtlas nya_sprite_atlas_grid(NYA_ConstCString texture, u32 frame_width, u32 frame_height) {
    return nya_sprite_atlas_grid_padded(texture, frame_width, frame_height, 0, 0);
}

NYA_SpriteAtlas nya_sprite_atlas_grid_padded(NYA_ConstCString texture, u32 frame_width, u32 frame_height, u32 spacing, u32 margin) {
    nya_assert(texture != nullptr);
    nya_assert(frame_width > 0 && frame_height > 0, "a sprite atlas needs a cell size");

    NYA_SpriteAtlas atlas = {
        .texture      = texture,
        .frame_width  = frame_width,
        .frame_height = frame_height,
        .spacing      = spacing,
        .margin       = margin,
    };

    // zero while the texture is still loading, and filled in on use once it has.
    _nya_sprite_atlas_grid_size(&atlas, &atlas.columns, &atlas.rows);

    return atlas;
}

u32 nya_sprite_atlas_frame_count(const NYA_SpriteAtlas* atlas) {
    nya_assert(atlas != nullptr);

    u32 columns, rows;
    _nya_sprite_atlas_grid_size(atlas, &columns, &rows);

    return columns * rows;
}

void nya_sprite_atlas_frame_rect(const NYA_SpriteAtlas* atlas, u32 frame, OUT f32* out_x, OUT f32* out_y, OUT f32* out_width, OUT f32* out_height) {
    nya_assert(atlas != nullptr);
    nya_assert(out_x != nullptr && out_y != nullptr && out_width != nullptr && out_height != nullptr);

    *out_x      = 0.0F;
    *out_y      = 0.0F;
    *out_width  = 0.0F;
    *out_height = 0.0F;

    // Out of range or no grid yet; a zeroed rectangle draws the whole texture, a visibly wrong frame rather than nothing.
    u32 columns, rows;
    _nya_sprite_atlas_grid_size(atlas, &columns, &rows);

    if (columns == 0 || frame >= columns * rows) return;

    u32 column = frame % columns;
    u32 row    = frame / columns;

    *out_x      = (f32)(atlas->margin + (column * (atlas->frame_width + atlas->spacing)));
    *out_y      = (f32)(atlas->margin + (row * (atlas->frame_height + atlas->spacing)));
    *out_width  = (f32)atlas->frame_width;
    *out_height = (f32)atlas->frame_height;
}

/*
 * ─────────────────────────────────────────────────────────
 * IMAGE LISTS
 * ─────────────────────────────────────────────────────────
 */

NYA_SpriteList nya_sprite_list(const NYA_ConstCString* textures, u32 count) {
    nya_assert(textures != nullptr || count == 0);

    return (NYA_SpriteList){ .textures = textures, .count = count };
}

u32 nya_sprite_list_frame_count(const NYA_SpriteList* list) {
    nya_assert(list != nullptr);

    return list->count;
}

NYA_Sprite nya_sprite_from_list(const NYA_SpriteList* list, u32 frame) {
    nya_assert(list != nullptr);

    NYA_Sprite sprite = {
        // Centred, like an atlas sprite: a frame is a character or object, which turns about its middle.
        .origin = { 0.5F, 0.5F },
        .scale  = { 1.0F, 1.0F },
        .tint   = NYA_COLOR_WHITE,
    };

    nya_sprite_set_frame_from_list(&sprite, list, frame);

    return sprite;
}

void nya_sprite_set_frame_from_list(OUT NYA_Sprite* sprite, const NYA_SpriteList* list, u32 frame) {
    nya_assert(sprite != nullptr);
    nya_assert(list != nullptr);

    // Out of range points at nothing, which draws nothing; clamping to the last frame would hide an animation past its end.
    sprite->texture = frame < list->count ? list->textures[frame] : nullptr;

    // Zeroed, which the drawing path reads as "the whole texture".
    sprite->source_x      = 0.0F;
    sprite->source_y      = 0.0F;
    sprite->source_width  = 0.0F;
    sprite->source_height = 0.0F;
}

/*
 * ─────────────────────────────────────────────────────────
 * SPRITES
 * ─────────────────────────────────────────────────────────
 */

NYA_Sprite nya_sprite_from_atlas(const NYA_SpriteAtlas* atlas, u32 frame) {
    nya_assert(atlas != nullptr);

    NYA_Sprite sprite = {
        .texture = atlas->texture,

        // Centred, since a sheet sprite is a character or object; a raw-rectangle sprite keeps the top left like every other draw.
        .origin = { 0.5F, 0.5F },
        .scale  = { 1.0F, 1.0F },
        .tint   = NYA_COLOR_WHITE,
    };

    nya_sprite_set_frame(&sprite, atlas, frame);

    return sprite;
}

NYA_Sprite nya_sprite_from_rect(NYA_ConstCString texture, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(texture != nullptr);

    return (NYA_Sprite){
        .texture      = texture,
        .source_x     = x,
        .source_y     = y,
        .source_width = width,
        .source_height = height,
        .scale        = { 1.0F, 1.0F },
        .tint         = NYA_COLOR_WHITE,
    };
}

void nya_sprite_set_frame(OUT NYA_Sprite* sprite, const NYA_SpriteAtlas* atlas, u32 frame) {
    nya_assert(sprite != nullptr);
    nya_assert(atlas != nullptr);

    // The texture comes along with the frame, so a sprite can be repointed at another sheet without updating both.
    sprite->texture = atlas->texture;

    nya_sprite_atlas_frame_rect(atlas, frame, &sprite->source_x, &sprite->source_y, &sprite->source_width, &sprite->source_height);
}

f32x2 nya_sprite_size(const NYA_Sprite* sprite) {
    nya_assert(sprite != nullptr);

    f32 width  = sprite->source_width;
    f32 height = sprite->source_height;

    // a zero source means the whole texture, whose size is only known once it has loaded.
    if (width <= 0.0F || height <= 0.0F) {
        u32 texture_width, texture_height;
        if (!_nya_sprite_texture_size(sprite->texture, &texture_width, &texture_height)) return f32x2_zero;

        width  = (f32)texture_width;
        height = (f32)texture_height;
    }

    f32 scale_x = sprite->scale.x != 0.0F ? sprite->scale.x : 1.0F;
    f32 scale_y = sprite->scale.y != 0.0F ? sprite->scale.y : 1.0F;

    return (f32x2){ width * scale_x, height * scale_y };
}

void nya_render2d_sprite(NYA_Window* window, const NYA_Sprite* sprite, f32x2 position) {
    nya_assert(window != nullptr);
    nya_assert(sprite != nullptr);

    f32x2 size = nya_sprite_size(sprite);

    // Nothing to draw when the texture is missing or loading; returning saves computing an origin for a zero size.
    if (size.x <= 0.0F || size.y <= 0.0F) return;

    nya_render2d_texture_ex(
        window,
        sprite->texture,
        (NYA_Render2DTexture){
            .source_x      = sprite->source_x,
            .source_y      = sprite->source_y,
            .source_width  = sprite->source_width,
            .source_height = sprite->source_height,

            .x = position.x,
            .y = position.y,

            .width  = size.x,
            .height = size.y,

            .rotation = sprite->rotation,

            // The pivot is a fraction here, pixels there; a fraction survives redrawing the sheet at another cell size.
            .origin = { sprite->origin.x * size.x, sprite->origin.y * size.y },

            .flip_x = sprite->flip_x,
            .flip_y = sprite->flip_y,

            .tint = sprite->tint,
        }
    );
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_sprite_atlas_grid_size(const NYA_SpriteAtlas* atlas, OUT u32* out_columns, OUT u32* out_rows) {
    *out_columns = atlas->columns;
    *out_rows    = atlas->rows;
    if (atlas->columns > 0 && atlas->rows > 0) return;

    u32 texture_width, texture_height;
    if (!_nya_sprite_texture_size(atlas->texture, &texture_width, &texture_height)) return;

    u32 usable_width  = texture_width > (atlas->margin * 2) ? texture_width - (atlas->margin * 2) : 0;
    u32 usable_height = texture_height > (atlas->margin * 2) ? texture_height - (atlas->margin * 2) : 0;

    // a cell costs its size plus the gap after it, and the last cell has no gap after it.
    *out_columns = (usable_width + atlas->spacing) / (atlas->frame_width + atlas->spacing);
    *out_rows    = (usable_height + atlas->spacing) / (atlas->frame_height + atlas->spacing);
}

b8 _nya_sprite_texture_size(NYA_ConstCString texture, OUT u32* out_width, OUT u32* out_height) {
    if (texture == nullptr) return false;

    // Cast because nya_asset_get takes a mutable handle while only reading it; every caller passes a literal.
    NYA_Asset* asset = nya_asset_get((NYA_CString)texture);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return false;

    *out_width  = asset->as_texture.width;
    *out_height = asset->as_texture.height;

    return *out_width > 0 && *out_height > 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * ANIMATION
 * ─────────────────────────────────────────────────────────
 */

/** Appends a signal if there is room. Silently drops past capacity; see NYA_SPRITE_ANIMATION_MAX_SIGNALS. */
NYA_INTERNAL void _nya_sprite_signal(OUT NYA_SpriteAnimationSignal* signals, u32 capacity, OUT u32* count, NYA_SpriteAnimationSignalKind kind,
                                     u32 frame, u32 id) {
    if (signals == nullptr || *count >= capacity) return;

    signals[*count] = (NYA_SpriteAnimationSignal){ .kind = kind, .frame = frame, .id = id };
    (*count)++;
}

/** Emits an EVENT signal for every marker sitting on `frame`. */
NYA_INTERNAL void _nya_sprite_events_for_frame(const NYA_SpriteAnimation* animation, u32 frame, OUT NYA_SpriteAnimationSignal* signals, u32 capacity,
                                               OUT u32* count) {
    if (animation->events == nullptr) return;

    u32 event_count = nya_min(animation->event_count, (u32)NYA_SPRITE_ANIMATION_MAX_EVENTS);

    // Every marker on the frame, not the first: two things can happen on one frame (a hit that also spawns dust).
    for (u32 i = 0; i < event_count; i++) {
        if (animation->events[i].frame == frame) {
            _nya_sprite_signal(signals, capacity, count, NYA_SPRITE_ANIMATION_EVENT, frame, animation->events[i].id);
        }
    }
}

void nya_sprite_animator_play(OUT NYA_SpriteAnimator* animator, const NYA_SpriteAnimation* animation) {
    nya_assert(animator != nullptr);

    *animator = (NYA_SpriteAnimator){
        .animation = animation,
        .speed     = 1.0F,
        .playing   = animation != nullptr,
        // Consumed by the first advance, so STARTED fires once and before any frame-zero event.
        .pending_start = animation != nullptr,
    };
}

void nya_sprite_animator_pause(OUT NYA_SpriteAnimator* animator) {
    nya_assert(animator != nullptr);

    animator->playing = false;
}

void nya_sprite_animator_resume(OUT NYA_SpriteAnimator* animator) {
    nya_assert(animator != nullptr);

    // Not for an animation that has ended: resuming would advance past the last frame and emit a second FINISHED (replay is nya_sprite_animator_play).
    if (animator->animation == nullptr || animator->finished) return;

    animator->playing = true;
}

void nya_sprite_animator_stop(OUT NYA_SpriteAnimator* animator) {
    nya_assert(animator != nullptr);

    *animator = (NYA_SpriteAnimator){ 0 };
}

u32 nya_sprite_animator_advance(OUT NYA_SpriteAnimator* animator, f32 delta_time_s, OUT NYA_SpriteAnimationSignal* out_signals, u32 capacity) {
    nya_assert(animator != nullptr);

    u32 count = 0;

    const NYA_SpriteAnimation* animation = animator->animation;
    if (animation == nullptr) return 0;

    if (animator->pending_start) {
        animator->pending_start = false;

        _nya_sprite_signal(out_signals, capacity, &count, NYA_SPRITE_ANIMATION_STARTED, 0, 0);

        // Frame zero is showing now, so a marker on it fires now rather than when the animation leaves it.
        _nya_sprite_events_for_frame(animation, 0, out_signals, capacity, &count);
    }

    if (!animator->playing || animator->finished) return count;
    if (animation->frame_count == 0) return count;

    nya_assert(animator->speed >= 0.0F, "a sprite animator plays forward only; got speed %f", (f64)animator->speed);

    f32 fps = animation->frames_per_second > 0.0F ? animation->frames_per_second : NYA_SPRITE_ANIMATION_DEFAULT_FPS;

    f32 seconds_per_frame = 1.0F / fps;

    animator->frame_elapsed_s += delta_time_s * animator->speed;

    /* Whole frames the accumulated time covers, by one divide, then each is walked. */
    u32 pending = (u32)(animator->frame_elapsed_s / seconds_per_frame);

    animator->frame_elapsed_s -= (f32)pending * seconds_per_frame;

    // Capped, so a huge delta from a breakpoint cannot walk millions of frames; the remainder is already dropped.
    if (pending > NYA_SPRITE_ANIMATION_MAX_SIGNALS) pending = NYA_SPRITE_ANIMATION_MAX_SIGNALS;

    for (u32 steps = 0; steps < pending; steps++) {

        if (animation->ping_pong) {
            if (animator->reversing) {
                if (animator->frame == 0) {
                    animator->reversing = false;

                    // A completed there-and-back is one loop, so a non-looping ping-pong stops here; the far end is halfway.
                    animator->loops++;

                    if (!animation->looping) {
                        animator->playing  = false;
                        animator->finished = true;
                        _nya_sprite_signal(out_signals, capacity, &count, NYA_SPRITE_ANIMATION_FINISHED, animator->frame, 0);
                        break;
                    }

                    _nya_sprite_signal(out_signals, capacity, &count, NYA_SPRITE_ANIMATION_LOOPED, 0, 0);
                } else {
                    animator->frame--;
                }
            } else if (animator->frame + 1 >= animation->frame_count) {
                animator->reversing = true;

                // Turn around at the end rather than show the last frame twice, which reads as a stutter.
                if (animator->frame > 0) animator->frame--;
            } else {
                animator->frame++;
            }
        } else if (animator->frame + 1 >= animation->frame_count) {
            if (!animation->looping) {
                animator->playing  = false;
                animator->finished = true;
                _nya_sprite_signal(out_signals, capacity, &count, NYA_SPRITE_ANIMATION_FINISHED, animator->frame, 0);
                break;
            }

            animator->frame = 0;
            animator->loops++;
            _nya_sprite_signal(out_signals, capacity, &count, NYA_SPRITE_ANIMATION_LOOPED, 0, 0);
        } else {
            animator->frame++;
        }

        _nya_sprite_events_for_frame(animation, animator->frame, out_signals, capacity, &count);
    }

    return count;
}

u32 nya_sprite_animator_frame(const NYA_SpriteAnimator* animator) {
    if (animator == nullptr || animator->animation == nullptr) return 0;

    return animator->animation->first_frame + animator->frame;
}

void nya_sprite_animator_apply(const NYA_SpriteAnimator* animator, const NYA_SpriteAtlas* atlas, OUT NYA_Sprite* sprite) {
    nya_assert(sprite != nullptr);

    if (animator == nullptr || atlas == nullptr || animator->animation == nullptr) return;

    nya_sprite_set_frame(sprite, atlas, nya_sprite_animator_frame(animator));
}
