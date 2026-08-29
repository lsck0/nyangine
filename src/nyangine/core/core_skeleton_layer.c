#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `bone` is `root` or lies below it. Walks parents, which is short — a rig is a few levels deep. */
NYA_INTERNAL b8 _nya_skeleton_is_descendant(const NYA_Skeleton* skeleton, s32 bone, s32 root) {
    for (s32 walk = bone; walk >= 0; walk = skeleton->bones[walk].parent) {
        if (walk == root) return true;
    }

    return false;
}

/** One bone's world-space position under `pose`, by composing the chain to the root. */
NYA_INTERNAL f32x3 _nya_skeleton_bone_position(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, s32 bone) {
    f32x3 position = { 0.0F, 0.0F, 0.0F };

    for (s32 walk = bone; walk >= 0; walk = skeleton->bones[walk].parent) {
        const NYA_BoneTransform* local = &pose->local[walk];

        // Up the chain, so each step takes the accumulated offset into the parent's frame.
        position = local->translation + nya_quaternion_rotate(local->rotation, position * local->scale);
    }

    return position;
}

/** The world-space rotation of a bone's parent, which is the frame a local rotation is expressed in. */
NYA_INTERNAL NYA_Quaternion _nya_skeleton_parent_rotation(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, s32 bone) {
    NYA_Quaternion rotation = nya_quaternion_identity;

    for (s32 walk = skeleton->bones[bone].parent; walk >= 0; walk = skeleton->bones[walk].parent) {
        rotation = nya_quaternion_multiply(pose->local[walk].rotation, rotation);
    }

    return rotation;
}

/**
 * How far `bone` moved in `clip` between two clock readings, following a loop across the seam.
 *
 * The seam is the whole difficulty. Playback went from `from_s` to the end and reappeared at zero,
 * so the naive `to - from` is the *negative* of a whole cycle: a walk cycle that loops once a second
 * teleports the character backwards once a second. Splitting the step at the seam and adding the two
 * halves is what makes a looping clip accumulate distance instead of oscillating.
 * */
NYA_INTERNAL NYA_RootMotion _nya_skeleton_root_step(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, s32 bone, f32 from_s, f32 to_s,
                                                    b8 wrapped, b8 forward) {
    if (clip == nullptr || bone < 0) return (NYA_RootMotion){ .rotation = nya_quaternion_identity };

    if (!wrapped) {
        NYA_BoneTransform a = nya_skeleton_clip_bone(skeleton, clip, bone, from_s);
        NYA_BoneTransform b = nya_skeleton_clip_bone(skeleton, clip, bone, to_s);

        return (NYA_RootMotion){
            .translation = b.translation - a.translation,
            .rotation    = nya_quaternion_multiply(nya_quaternion_inverse(a.rotation), b.rotation),
        };
    }

    // The two ends the step ran between, in playback order. Backwards playback wraps the other way:
    // it leaves through zero and comes back at the duration.
    f32 leave  = forward ? clip->duration_s : 0.0F;
    f32 arrive = forward ? 0.0F : clip->duration_s;

    NYA_BoneTransform a       = nya_skeleton_clip_bone(skeleton, clip, bone, from_s);
    NYA_BoneTransform at_end  = nya_skeleton_clip_bone(skeleton, clip, bone, leave);
    NYA_BoneTransform restart = nya_skeleton_clip_bone(skeleton, clip, bone, arrive);
    NYA_BoneTransform b       = nya_skeleton_clip_bone(skeleton, clip, bone, to_s);

    return (NYA_RootMotion){
        .translation = (at_end.translation - a.translation) + (b.translation - restart.translation),
        .rotation    = nya_quaternion_multiply(nya_quaternion_multiply(nya_quaternion_inverse(a.rotation), at_end.rotation),
                                               nya_quaternion_multiply(nya_quaternion_inverse(restart.rotation), b.rotation)),
    };
}

/** Whether an animator's clock crossed the loop seam this step, given which way it is running. */
NYA_INTERNAL b8 _nya_skeleton_wrapped(const NYA_SkeletonAnimator* animator, f32 before_s, b8 forward) {
    if (!animator->looping) return false;

    return forward ? animator->time_s < before_s : animator->time_s > before_s;
}

/** Puts the extracted axes of `bone` back where the rest pose has them, so the character stays put. */
NYA_INTERNAL void _nya_skeleton_pin_root(const NYA_Skeleton* skeleton, NYA_SkeletonPose* pose, s32 bone, f32x3 axes, b8 rotation) {
    if (bone < 0 || (u32)bone >= pose->bone_count) return;

    const NYA_BoneTransform* rest = &skeleton->bones[bone].rest;

    // Per component rather than a branch per axis: `axes` is a 0-or-1 multiplier, so this is a select
    // where it is either, and a partial extraction where somebody sets it to 0.5.
    pose->local[bone].translation += (rest->translation - pose->local[bone].translation) * axes;

    if (rotation) pose->local[bone].rotation = rest->rotation;
}

/** Fires any event the last step crossed, in time order. Handles a loop wrapping past the end. */
NYA_INTERNAL void _nya_skeleton_collect_events(NYA_SkeletonPlayer* player, f32 from_s, f32 to_s, b8 looped, f32 duration_s) {
    if (player->events == nullptr || player->event_count == 0) return;

    for (u32 i = 0; i < player->event_count; i++) {
        const NYA_SkeletonEvent* event = &player->events[i];

        /*
         * A loop splits the step into two intervals rather than one.
         *
         * Playback went from `from_s` to the end and then from zero to `to_s`, so an event between them
         * is in neither if the interval is treated as [from, to] — which is how a footstep near the end
         * of a walk cycle silently stops firing once the clip loops.
         */
        b8 crossed = looped ? (event->time_s > from_s && event->time_s <= duration_s) || (event->time_s >= 0.0F && event->time_s <= to_s)
                            : (event->time_s > from_s && event->time_s <= to_s);

        if (!crossed) continue;
        if (player->signal_count >= NYA_SKELETON_CLIP_EVENTS) return;

        player->signals[player->signal_count++] = (NYA_SkeletonSignal){
            .kind = NYA_SKELETON_SIGNAL_EVENT,
            .id   = event->id,
            .clip = player->current.clip,
        };
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_skeleton_mask_fill(const NYA_Skeleton* skeleton, f32 weight, OUT NYA_SkeletonMask* out_mask) {
    if (skeleton == nullptr || out_mask == nullptr) return;

    *out_mask = (NYA_SkeletonMask){ .bone_count = skeleton->bone_count };

    for (u32 i = 0; i < skeleton->bone_count && i < NYA_SKELETON_MAX_BONES; i++) out_mask->weights[i] = nya_clamp(weight, 0.0F, 1.0F);
}

b8 nya_skeleton_mask_from_bone(const NYA_Skeleton* skeleton, NYA_ConstCString root, OUT NYA_SkeletonMask* out_mask) {
    if (skeleton == nullptr || out_mask == nullptr) return false;

    s32 index = nya_skeleton_bone_index(skeleton, root);

    // Reported rather than producing an empty mask: a layer through an all-zero mask does nothing, and
    // "the animation is not playing" is a much harder thing to trace back to a misspelled bone name.
    if (index < 0) return false;

    nya_skeleton_mask_fill(skeleton, 0.0F, out_mask);
    nya_skeleton_mask_set(skeleton, out_mask, index, 1.0F, true);

    return true;
}

void nya_skeleton_mask_set(const NYA_Skeleton* skeleton, NYA_SkeletonMask* mask, s32 bone, f32 weight, b8 include_descendants) {
    if (skeleton == nullptr || mask == nullptr || bone < 0 || (u32)bone >= skeleton->bone_count) return;

    weight = nya_clamp(weight, 0.0F, 1.0F);

    if (!include_descendants) {
        mask->weights[bone] = weight;
        return;
    }

    for (u32 i = 0; i < skeleton->bone_count && i < NYA_SKELETON_MAX_BONES; i++) {
        if (_nya_skeleton_is_descendant(skeleton, (s32)i, bone)) mask->weights[i] = weight;
    }
}

void nya_skeleton_pose_blend_masked(const NYA_SkeletonPose* from, const NYA_SkeletonPose* to, f32 amount, const NYA_SkeletonMask* mask,
                                    OUT NYA_SkeletonPose* out_pose) {
    if (from == nullptr || to == nullptr || out_pose == nullptr) return;

    u32 bones = nya_min(from->bone_count, to->bone_count);

    out_pose->bone_count = bones;

    for (u32 i = 0; i < bones; i++) {
        f32 weight = mask != nullptr ? mask->weights[i] : 1.0F;
        f32 t      = nya_clamp(amount * weight, 0.0F, 1.0F);

        // Skipped rather than blended at zero: a slerp of a bone that is not in the mask is the bulk of
        // a masked blend's cost, and it is the whole reason a mask is worth having.
        if (t <= 0.0F) {
            out_pose->local[i] = from->local[i];
            continue;
        }

        out_pose->local[i] = (NYA_BoneTransform){
            .translation = nya_lerp(from->local[i].translation, to->local[i].translation, t),
            .rotation    = nya_quaternion_slerp_unit(from->local[i].rotation, to->local[i].rotation, t),
            .scale       = nya_lerp(from->local[i].scale, to->local[i].scale, t),
        };
    }
}

void nya_skeleton_player_init(NYA_SkeletonPlayer* player, const NYA_Skeleton* skeleton) {
    if (player == nullptr) return;

    // Off is -1, and zero would be bone zero, which on a humanoid rig is exactly the bone somebody
    // would have meant. A designated initialiser cannot express that, hence the assignment after.
    *player                   = (NYA_SkeletonPlayer){ .skeleton = skeleton };
    player->root_motion_bone  = -1;
    player->root_motion.rotation = nya_quaternion_identity;
}

void nya_skeleton_player_play_with_options(NYA_SkeletonPlayer* player, const NYA_SkeletonClip* clip, NYA_SkeletonPlayOptions options) {
    if (player == nullptr || clip == nullptr || player->skeleton == nullptr) return;

    // Already playing this, and not asked to restart. Lets a caller drive this from a state check every
    // frame — "if running, play the run clip" — without restarting the animation every frame.
    if (player->current.clip == clip && !options.restart) return;

    if (options.speed == 0.0F) options.speed = 1.0F;

    if (player->inertializer != nullptr && player->current.clip != nullptr && options.fade_s > 0.0F) {
        /*
         * The outgoing clip is dropped here and now, which is the whole saving.
         *
         * A crossfade would keep it and evaluate it for the length of the transition; the inertializer
         * only needs the pose that was already on screen, and it has been recording that itself. What
         * it cannot do yet is measure the destination's velocity, so the capture waits for the update
         * that knows the frame delta. See NYA_SkeletonPlayer.pending_inertial_s.
         */
        player->pending_inertial_s = options.fade_s;
        player->fading             = false;
    } else if (player->current.clip != nullptr && options.fade_s > 0.0F) {
        // The outgoing clip keeps playing during the fade, which is what makes it a crossfade rather
        // than a dissolve into a frozen pose.
        player->previous        = player->current;
        player->fade_elapsed_s  = 0.0F;
        player->fade_duration_s = options.fade_s;
        player->fading          = true;
    } else {
        player->fading = false;
    }

    nya_skeleton_animator_play(&player->current, player->skeleton, clip, options.looping);
    player->current.speed = options.speed;
}

void nya_skeleton_player_events(NYA_SkeletonPlayer* player, const NYA_SkeletonEvent* events, u32 count) {
    if (player == nullptr) return;

    player->events      = events;
    player->event_count = events != nullptr ? count : 0;
}

b8 nya_skeleton_player_layer(NYA_SkeletonPlayer* player, u32 slot, const NYA_SkeletonClip* clip, const NYA_SkeletonMask* mask, f32 weight,
                             b8 looping) {
    if (player == nullptr || clip == nullptr || slot >= NYA_SKELETON_LAYERS || player->skeleton == nullptr) return false;

    NYA_SkeletonLayer* layer = &player->layers[slot];

    *layer = (NYA_SkeletonLayer){ .mask = mask, .weight = nya_clamp(weight, 0.0F, 1.0F), .active = true };

    nya_skeleton_animator_play(&layer->animator, player->skeleton, clip, looping);

    return true;
}

void nya_skeleton_player_layer_stop(NYA_SkeletonPlayer* player, u32 slot) {
    if (player == nullptr || slot >= NYA_SKELETON_LAYERS) return;

    player->layers[slot] = (NYA_SkeletonLayer){ 0 };
}

void nya_skeleton_player_layer_weight(NYA_SkeletonPlayer* player, u32 slot, f32 weight) {
    if (player == nullptr || slot >= NYA_SKELETON_LAYERS) return;

    player->layers[slot].weight = nya_clamp(weight, 0.0F, 1.0F);
}

b8 nya_skeleton_player_root_motion(NYA_SkeletonPlayer* player, NYA_ConstCString bone, f32x3 translation_axes, b8 rotation) {
    if (player == nullptr || player->skeleton == nullptr) return false;

    if (bone == nullptr) {
        player->root_motion_bone = -1;
        player->root_motion      = (NYA_RootMotion){ .rotation = nya_quaternion_identity };
        return true;
    }

    s32 index = nya_skeleton_bone_index(player->skeleton, bone);

    // Reported rather than left off, for the same reason nya_skeleton_mask_from_bone reports: a
    // character that does not move because a bone name was misspelled is a long afternoon.
    if (index < 0) return false;

    player->root_motion_bone     = index;
    player->root_motion_axes     = translation_axes;
    player->root_motion_rotation = rotation;
    player->root_motion          = (NYA_RootMotion){ .rotation = nya_quaternion_identity };

    return true;
}

NYA_RootMotion nya_skeleton_player_root_delta(const NYA_SkeletonPlayer* player) {
    if (player == nullptr) return (NYA_RootMotion){ .rotation = nya_quaternion_identity };

    return player->root_motion;
}

void nya_skeleton_player_inertial(NYA_SkeletonPlayer* player, NYA_SkeletonInertializer* inertializer) {
    if (player == nullptr) return;

    player->inertializer = inertializer;
}

b8 nya_skeleton_player_fading(const NYA_SkeletonPlayer* player) {
    if (player == nullptr) return false;

    return player->fading || player->pending_inertial_s > 0.0F || nya_skeleton_inertializer_active(player->inertializer);
}

void nya_skeleton_player_update(NYA_SkeletonPlayer* player, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose) {
    if (player == nullptr || out_pose == nullptr || player->skeleton == nullptr) return;

    player->signal_count = 0;

    if (player->current.clip == nullptr) {
        nya_skeleton_pose_rest(player->skeleton, out_pose);

        // Cleared rather than left alone: a caller applies this every frame, and a stale delta from
        // the last clip would keep pushing the character after the animation stopped.
        player->root_motion = (NYA_RootMotion){ .rotation = nya_quaternion_identity };

        // The inertializer still runs, so that its history is the rest pose rather than whatever was
        // on screen before the clip was taken away. A `play` after an idle gap would otherwise
        // transition from a pose the character has not been in for some time.
        if (player->inertializer != nullptr) nya_skeleton_inertializer_update(player->inertializer, delta_time_s, out_pose);

        return;
    }

    f32 before_s          = player->current.time_s;
    f32 before_previous_s = player->previous.time_s;
    b8  was_ended         = player->current.finished;

    b8 extracting = player->root_motion_bone >= 0;

    nya_skeleton_animator_update(&player->current, delta_time_s, out_pose);

    f32 after_s  = player->current.time_s;
    b8  looped   = player->current.looping && after_s < before_s;
    f32 duration = player->current.clip->duration_s;

    _nya_skeleton_collect_events(player, before_s, after_s, looped, duration);

    if (looped && player->signal_count < NYA_SKELETON_CLIP_EVENTS) {
        player->signals[player->signal_count++] = (NYA_SkeletonSignal){ .kind = NYA_SKELETON_SIGNAL_LOOPED, .clip = player->current.clip };
    }

    if (player->current.finished && !was_ended && player->signal_count < NYA_SKELETON_CLIP_EVENTS) {
        player->signals[player->signal_count++] = (NYA_SkeletonSignal){ .kind = NYA_SKELETON_SIGNAL_FINISHED, .clip = player->current.clip };
    }

    /*
     * ── Root motion ──
     *
     * Measured from the clocks the update above just moved, so it is the same interval the pose came
     * from. The pin happens at the very end, after the layers, because a layer is allowed to write
     * the root too and pinning before that would only be undone.
     */
    if (extracting) {
        b8 forward = player->current.speed >= 0.0F;

        player->root_motion = _nya_skeleton_root_step(player->skeleton, player->current.clip, player->root_motion_bone, before_s,
                                                      player->current.time_s, _nya_skeleton_wrapped(&player->current, before_s, forward), forward);
    }

    /*
     * ── The crossfade ──
     *
     * The outgoing clip is advanced too, so both are moving during the transition. Blending toward a
     * pose frozen at the moment of the switch is the cheap version and reads as a stutter.
     */
    if (player->fading) {
        NYA_SkeletonPose outgoing = { 0 };
        nya_skeleton_animator_update(&player->previous, delta_time_s, &outgoing);

        player->fade_elapsed_s += delta_time_s;

        f32 t = player->fade_duration_s > 0.0F ? nya_clamp(player->fade_elapsed_s / player->fade_duration_s, 0.0F, 1.0F) : 1.0F;

        // Smoothstepped, not linear: a linear crossfade has a velocity discontinuity at each end, which
        // is visible as a twitch when the two clips disagree about where a limb is.
        f32 eased = nya_ease(NYA_EASE_SMOOTHSTEP, t);

        NYA_SkeletonPose blended = { 0 };
        nya_skeleton_pose_blend(&outgoing, out_pose, eased, &blended);
        *out_pose = blended;

        /*
         * The outgoing clip's travel, mixed in by the same curve as the pose.
         *
         * Without this a walk-to-run transition steps between two speeds on the frame the clip
         * changes, while the *pose* eases across the whole fade — the character's feet and its
         * position disagree for as long as the transition lasts, which is what foot sliding is.
         */
        if (extracting) {
            b8 previous_forward = player->previous.speed >= 0.0F;

            NYA_RootMotion outgoing_motion =
                _nya_skeleton_root_step(player->skeleton, player->previous.clip, player->root_motion_bone, before_previous_s,
                                        player->previous.time_s, _nya_skeleton_wrapped(&player->previous, before_previous_s, previous_forward),
                                        previous_forward);

            player->root_motion = (NYA_RootMotion){
                .translation = nya_lerp(outgoing_motion.translation, player->root_motion.translation, eased),
                .rotation    = nya_quaternion_slerp_unit(outgoing_motion.rotation, player->root_motion.rotation, eased),
            };
        }

        if (t >= 1.0F) {
            player->fading   = false;
            player->previous = (NYA_SkeletonAnimator){ 0 };
        }
    }

    /*
     * ── The layers, in order ──
     *
     * Each one blends over the result of those below it, so a later layer wins where their masks
     * overlap. That is what "layers stack" means and it is why the order is the slot order.
     */
    for (u32 i = 0; i < NYA_SKELETON_LAYERS; i++) {
        NYA_SkeletonLayer* layer = &player->layers[i];
        if (!layer->active || layer->animator.clip == nullptr || layer->weight <= 0.0F) continue;

        NYA_SkeletonPose layer_pose = { 0 };
        nya_skeleton_animator_update(&layer->animator, delta_time_s, &layer_pose);

        NYA_SkeletonPose composed = { 0 };
        nya_skeleton_pose_blend_masked(out_pose, &layer_pose, layer->weight, layer->mask, &composed);

        *out_pose = composed;
    }

    /*
     * ── Inertialization, after the layers and before the pin ──
     *
     * After the layers because the offset has to be measured against the pose that was actually shown,
     * layers included — that is what makes a second transition during a first one compose. Before the
     * pin because the pin *overwrites* the root's extracted axes: an offset added afterwards would put
     * the character back to drifting, which is the one thing root motion exists to stop.
     */
    if (player->inertializer != nullptr) {
        if (player->pending_inertial_s > 0.0F) {
            /*
             * The destination one frame back, so the capture can tell how fast the *new* clip is
             * already moving. Without it a switch between two clips that swing a limb the same way
             * arrives carrying the sum of both velocities rather than the difference.
             *
             * The base clip rather than the fully composed pose: recomposing the layers at t - Δ would
             * cost exactly the second evaluation this technique exists to avoid, and the base clip is
             * what a transition changes. A layer's own motion goes unaccounted for, which shows up as
             * a slightly early arrival on the bones that layer owns.
             */
            f32 previous_time_s = player->current.time_s - (delta_time_s * player->current.speed);

            if (player->current.looping && player->current.clip->duration_s > 0.0F) {
                previous_time_s = fmodf(previous_time_s, player->current.clip->duration_s);
                if (previous_time_s < 0.0F) previous_time_s += player->current.clip->duration_s;
            }

            NYA_SkeletonPose destination_previous = { 0 };
            nya_skeleton_pose_sample(player->skeleton, player->current.clip, previous_time_s, &destination_previous);

            nya_skeleton_inertializer_transition(player->inertializer, out_pose, &destination_previous, player->pending_inertial_s);

            player->pending_inertial_s = 0.0F;
        }

        nya_skeleton_inertializer_update(player->inertializer, delta_time_s, out_pose);
    }

    /*
     * Last, because everything above is allowed to write the root and this is what decides it does
     * not get to keep it. See nya_skeleton_player_root_motion for why the rest pose is the target.
     *
     * The report is masked by the same axes as the pin, and for the same reason. What is not
     * extracted stays in the pose, so reporting it as well would hand the caller a displacement the
     * animation is still applying — a walk cycle's vertical bob turned into a character that hops.
     */
    if (extracting) {
        player->root_motion.translation *= player->root_motion_axes;
        if (!player->root_motion_rotation) player->root_motion.rotation = nya_quaternion_identity;

        _nya_skeleton_pin_root(player->skeleton, out_pose, player->root_motion_bone, player->root_motion_axes, player->root_motion_rotation);
    }
}

b8 nya_skeleton_ik_two_bone(const NYA_Skeleton* skeleton, NYA_SkeletonPose* pose, s32 root_bone, s32 mid_bone, s32 end_bone, f32x3 target,
                            f32x3 pole) {
    if (skeleton == nullptr || pose == nullptr) return false;
    if (root_bone < 0 || mid_bone < 0 || end_bone < 0) return false;
    if ((u32)root_bone >= pose->bone_count || (u32)mid_bone >= pose->bone_count || (u32)end_bone >= pose->bone_count) return false;

    // A chain, not three arbitrary bones: the maths below is a triangle and means nothing otherwise.
    if (skeleton->bones[end_bone].parent != mid_bone || skeleton->bones[mid_bone].parent != root_bone) return false;

    f32x3 root = _nya_skeleton_bone_position(skeleton, pose, root_bone);
    f32x3 mid  = _nya_skeleton_bone_position(skeleton, pose, mid_bone);
    f32x3 end  = _nya_skeleton_bone_position(skeleton, pose, end_bone);

    f32 upper = nya_vector_length(mid - root);
    f32 lower = nya_vector_length(end - mid);

    if (upper <= NYA_EPSILON || lower <= NYA_EPSILON) return false;

    f32x3 to_target = target - root;
    f32   reach     = nya_vector_length(to_target);

    if (reach <= NYA_EPSILON) return false;

    f32x3 direction = to_target / reach;

    /*
     * Clamped just inside full extension.
     *
     * At exactly `upper + lower` the triangle is degenerate and the joint's bend direction is undefined,
     * so a limb reaching for something barely too far flickers between bending each way. Stopping a
     * hair short keeps the plane defined, and the visible result — an almost straight limb — is what a
     * real one does anyway.
     */
    f32 clamped = nya_min(reach, (upper + lower) * 0.999F);

    // Law of cosines: the angle at the root between the target line and the upper bone.
    f32 cos_root = ((upper * upper) + (clamped * clamped) - (lower * lower)) / (2.0F * upper * clamped);
    cos_root     = nya_clamp(cos_root, -1.0F, 1.0F);

    f32 root_angle = acosf(cos_root);

    /*
     * The bend plane, from the pole.
     *
     * Without a pole the solution is a circle of valid elbow positions around the root-to-target line,
     * and nothing chooses between them — the limb spins. The pole names the direction the joint should
     * point, and the component of it perpendicular to the target line is the plane's second axis.
     */
    f32x3 pole_direction = pole - root;
    f32x3 perpendicular  = pole_direction - (direction * nya_vector_dot(pole_direction, direction));

    if (nya_vector_length(perpendicular) <= NYA_EPSILON) {
        // A pole on the target line says nothing. Any perpendicular will do, so take one that is not
        // parallel to the direction rather than refusing.
        f32x3 fallback = fabsf(direction.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };
        perpendicular  = fallback - (direction * nya_vector_dot(fallback, direction));
    }

    perpendicular = nya_vector_normalize(perpendicular);

    // Where the joint has to be for the triangle to close.
    f32x3 mid_target = root + (direction * (cosf(root_angle) * upper)) + (perpendicular * (sinf(root_angle) * upper));

    /*
     * Turned into local rotations by rotating each bone from where it points to where it should.
     *
     * A rotation between two directions rather than a constructed basis: the bone's roll around its own
     * axis is whatever the animation set, and rebuilding a basis here would discard it.
     */
    NYA_Quaternion root_parent = _nya_skeleton_parent_rotation(skeleton, pose, root_bone);
    NYA_Quaternion swing_root  = nya_quaternion_from_to(nya_vector_normalize(mid - root), nya_vector_normalize(mid_target - root));

    pose->local[root_bone].rotation =
        nya_quaternion_normalize(nya_quaternion_multiply(nya_quaternion_multiply(nya_quaternion_inverse(root_parent), swing_root),
                                                         nya_quaternion_multiply(root_parent, pose->local[root_bone].rotation)));

    // Recomputed after the root moved, so the second bone aims from where the joint now is.
    f32x3 new_end = _nya_skeleton_bone_position(skeleton, pose, end_bone);
    f32x3 new_mid = _nya_skeleton_bone_position(skeleton, pose, mid_bone);

    NYA_Quaternion mid_parent = _nya_skeleton_parent_rotation(skeleton, pose, mid_bone);
    NYA_Quaternion swing_mid  = nya_quaternion_from_to(nya_vector_normalize(new_end - new_mid), nya_vector_normalize(target - new_mid));

    pose->local[mid_bone].rotation =
        nya_quaternion_normalize(nya_quaternion_multiply(nya_quaternion_multiply(nya_quaternion_inverse(mid_parent), swing_mid),
                                                         nya_quaternion_multiply(mid_parent, pose->local[mid_bone].rotation)));

    return true;
}
