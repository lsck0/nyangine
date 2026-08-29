#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Solves the decay curve for one channel, given where the offset starts and how fast it is moving.
 *
 * The polynomial is the one from Bollo's GDC 2016 talk: fifth order, pinned so that x, x' and x'' are
 * all zero at t₁, which is what makes the transition *end* rather than fade out forever. Its three
 * free coefficients follow from those three conditions; the remaining three are the initial value,
 * velocity and acceleration.
 *
 * Two choices in here are the ones that keep it from misbehaving, and both are Bollo's:
 *
 * - **a₀ is not free.** Setting it to `(-8v₀t₁ - 20x₀) / t₁²` is what stops the curve crossing zero
 *   before t₁ and coming back — an offset that overshoots is a limb that swings past where it was
 *   going and returns, which reads worse than the pop the transition existed to remove.
 * - **t₁ is shortened when the offset is already closing.** If v₀ is negative the gap is shrinking on
 *   its own and will reach zero before the requested duration; `-5x₀/v₀` is where, and running the
 *   curve past that point would mean pulling the offset back open to spend the rest of the budget.
 *
 * Measured over v₀t₁/x₀ from -5 to 16, the curve never goes below zero and peaks at 2.1× x₀ at the top
 * of that range. It only rises at all when the two poses are actively separating, which is exactly
 * when matching velocity requires it.
 * */
NYA_INTERNAL void _nya_inertial_solve(NYA_InertialChannel* channel, f32x3 direction, f32 x0, f32 v0, f32 duration_s) {
    *channel = (NYA_InertialChannel){ .direction = direction };

    if (x0 <= NYA_EPSILON || duration_s <= NYA_EPSILON) return;

    f32 t1 = duration_s;

    // Already closing, and fast enough to arrive early. See the note above.
    if (v0 < 0.0F) {
        f32 natural = -5.0F * x0 / v0;
        if (natural < t1) t1 = natural;
    }

    if (t1 <= NYA_EPSILON) return;

    f32 a0 = ((-8.0F * v0 * t1) - (20.0F * x0)) / (t1 * t1);

    f32 t1_2 = t1 * t1;
    f32 t1_3 = t1_2 * t1;
    f32 t1_4 = t1_3 * t1;
    f32 t1_5 = t1_4 * t1;

    channel->coefficients[0] = -((a0 * t1_2) + (6.0F * v0 * t1) + (12.0F * x0)) / (2.0F * t1_5);
    channel->coefficients[1] = ((3.0F * a0 * t1_2) + (16.0F * v0 * t1) + (30.0F * x0)) / (2.0F * t1_4);
    channel->coefficients[2] = -((3.0F * a0 * t1_2) + (12.0F * v0 * t1) + (20.0F * x0)) / (2.0F * t1_3);
    channel->coefficients[3] = a0 * 0.5F;
    channel->coefficients[4] = v0;
    channel->coefficients[5] = x0;

    channel->duration_s = t1;
}

/** The offset's length at `t`, or zero once the channel has arrived. Horner, so five multiply-adds. */
NYA_INTERNAL f32 _nya_inertial_evaluate(const NYA_InertialChannel* channel, f32 t) {
    if (t >= channel->duration_s) return 0.0F;

    const f32* c = channel->coefficients;

    return (((((c[0] * t) + c[1]) * t + c[2]) * t + c[3]) * t + c[4]) * t + c[5];
}

/*
 * There is deliberately no "velocity of the decay curve" helper here.
 *
 * A transition that fires while one is still running would seem to need it, to carry the offset's
 * current rate of change into the new curve. It does not: the history the capture reads is the pose
 * *after* the offset was added, so the offset's velocity is already inside the difference between the
 * last two frames. Differentiating the polynomial as well would count it twice.
 */

/** A rotation as the axis-angle vector `axis * angle`, which is what can be differenced and scaled. */
NYA_INTERNAL f32x3 _nya_inertial_rotation_vector(NYA_Quaternion rotation) {
    f32x3 axis  = f32x3_unit_x;
    f32   angle = 0.0F;

    nya_quaternion_to_axis_angle(rotation, &axis, &angle);

    return axis * angle;
}

/** Captures one vector channel — translation or scale, which behave identically. */
NYA_INTERNAL f32 _nya_inertial_capture_vector(NYA_InertialChannel* channel, f32x3 source, f32x3 source_previous, f32x3 target,
                                              f32x3 target_previous, f32 delta_s, f32 duration_s) {
    f32x3 offset = source - target;
    f32   x0     = nya_vector_length(offset);

    if (x0 <= NYA_EPSILON) {
        *channel = (NYA_InertialChannel){ .direction = f32x3_unit_x };
        return 0.0F;
    }

    f32x3 direction = offset / x0;

    /*
     * The offset's velocity, projected onto the direction it will decay along.
     *
     * Both halves matter. The source's velocity alone would leave the destination's own motion
     * unaccounted for, so a character switching between two clips that move a limb the same way would
     * arrive carrying the sum of both. The difference is what the seam actually has to be continuous in.
     */
    f32 v0 = 0.0F;
    if (delta_s > NYA_EPSILON) {
        f32x3 source_velocity = (source - source_previous) / delta_s;
        f32x3 target_velocity = (target - target_previous) / delta_s;

        v0 = nya_vector_dot(source_velocity - target_velocity, direction);
    }

    _nya_inertial_solve(channel, direction, x0, v0, duration_s);

    return channel->duration_s;
}

/** Captures the rotation channel. The same shape as the vector case, in axis-angle rather than metres. */
NYA_INTERNAL f32 _nya_inertial_capture_rotation(NYA_InertialChannel* channel, NYA_Quaternion source, NYA_Quaternion source_previous,
                                                NYA_Quaternion target, NYA_Quaternion target_previous, f32 delta_s, f32 duration_s) {
    // The rotation that takes the destination to where the character currently is, which is the offset
    // to remove: applying it to the destination pose reproduces the source pose exactly.
    NYA_Quaternion offset = nya_quaternion_multiply(source, nya_quaternion_inverse(target));

    f32x3 axis  = f32x3_unit_x;
    f32   angle = 0.0F;
    nya_quaternion_to_axis_angle(offset, &axis, &angle);

    if (angle <= NYA_EPSILON) {
        *channel = (NYA_InertialChannel){ .direction = f32x3_unit_x };
        return 0.0F;
    }

    f32 v0 = 0.0F;
    if (delta_s > NYA_EPSILON) {
        // Angular velocities as axis-angle vectors, which subtract the way linear ones do for the small
        // per-frame rotations this deals with.
        f32x3 source_velocity = _nya_inertial_rotation_vector(nya_quaternion_multiply(source, nya_quaternion_inverse(source_previous))) / delta_s;
        f32x3 target_velocity = _nya_inertial_rotation_vector(nya_quaternion_multiply(target, nya_quaternion_inverse(target_previous))) / delta_s;

        v0 = nya_vector_dot(source_velocity - target_velocity, axis);
    }

    _nya_inertial_solve(channel, axis, angle, v0, duration_s);

    return channel->duration_s;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_skeleton_inertializer_init(NYA_SkeletonInertializer* inertializer, const NYA_Skeleton* skeleton) {
    if (inertializer == nullptr) return;

    nya_memset(inertializer, 0, sizeof(*inertializer));
    inertializer->skeleton = skeleton;
}

void nya_skeleton_inertializer_transition(NYA_SkeletonInertializer* inertializer, const NYA_SkeletonPose* target,
                                          const NYA_SkeletonPose* target_previous, f32 duration_s) {
    if (inertializer == nullptr || target == nullptr || inertializer->skeleton == nullptr) return;
    if (duration_s <= NYA_EPSILON) return;

    /*
     * Nothing has been shown yet, so there is nothing to transition *from*.
     *
     * Silently doing nothing rather than capturing an offset against a zeroed pose: the first frame of
     * a character's life is a cut, and inventing a difference against an empty history would throw the
     * whole rig at the origin and then decay it back over the transition.
     */
    if (inertializer->history_frames == 0) return;

    // One frame of history gives a pose but no velocity, which the capture handles by taking v₀ as zero.
    f32 delta_s = inertializer->history_frames >= 2 ? inertializer->previous_delta_s : 0.0F;

    u32 bones = nya_min(target->bone_count, inertializer->previous.bone_count);
    if (bones > NYA_SKELETON_MAX_BONES) bones = NYA_SKELETON_MAX_BONES;

    f32 longest = 0.0F;

    for (u32 i = 0; i < bones; i++) {
        const NYA_BoneTransform* source          = &inertializer->previous.local[i];
        const NYA_BoneTransform* source_previous = &inertializer->before_previous.local[i];
        const NYA_BoneTransform* destination     = &target->local[i];

        // A null target_previous means "the destination is standing still", which zeroes its half of
        // the relative velocity rather than special casing the arithmetic below.
        const NYA_BoneTransform* destination_previous = target_previous != nullptr && i < target_previous->bone_count
                                                          ? &target_previous->local[i]
                                                          : destination;

        f32 translation_s = _nya_inertial_capture_vector(&inertializer->translation[i], source->translation, source_previous->translation,
                                                         destination->translation, destination_previous->translation, delta_s, duration_s);

        f32 rotation_s = _nya_inertial_capture_rotation(&inertializer->rotation[i], source->rotation, source_previous->rotation,
                                                        destination->rotation, destination_previous->rotation, delta_s, duration_s);

        f32 scale_s = _nya_inertial_capture_vector(&inertializer->scale[i], source->scale, source_previous->scale, destination->scale,
                                                   destination_previous->scale, delta_s, duration_s);

        longest = nya_max(longest, nya_max(translation_s, nya_max(rotation_s, scale_s)));
    }

    // Bones the target does not have keep no offset, or they would decay against whatever was left in
    // the array from a previous, larger skeleton.
    for (u32 i = bones; i < NYA_SKELETON_MAX_BONES; i++) {
        inertializer->translation[i] = (NYA_InertialChannel){ .direction = f32x3_unit_x };
        inertializer->rotation[i]    = (NYA_InertialChannel){ .direction = f32x3_unit_x };
        inertializer->scale[i]       = (NYA_InertialChannel){ .direction = f32x3_unit_x };
    }

    inertializer->elapsed_s = 0.0F;
    inertializer->longest_s = longest;
}

void nya_skeleton_inertializer_update(NYA_SkeletonInertializer* inertializer, f32 delta_time_s, NYA_SkeletonPose* pose) {
    if (inertializer == nullptr || pose == nullptr) return;

    if (nya_skeleton_inertializer_active(inertializer)) {
        inertializer->elapsed_s += delta_time_s;

        f32 t = inertializer->elapsed_s;

        u32 bones = nya_min(pose->bone_count, (u32)NYA_SKELETON_MAX_BONES);

        for (u32 i = 0; i < bones; i++) {
            NYA_BoneTransform* local = &pose->local[i];

            f32 translation = _nya_inertial_evaluate(&inertializer->translation[i], t);
            if (translation != 0.0F) local->translation += inertializer->translation[i].direction * translation;

            f32 rotation = _nya_inertial_evaluate(&inertializer->rotation[i], t);
            if (rotation != 0.0F) {
                // Left multiplied, matching how the offset was captured: offset ⋅ destination is the
                // pose that was on screen, so the same product reproduces it at t = 0.
                local->rotation = nya_quaternion_normalize(
                    nya_quaternion_multiply(nya_quaternion_from_axis_angle(inertializer->rotation[i].direction, rotation), local->rotation));
            }

            f32 scale = _nya_inertial_evaluate(&inertializer->scale[i], t);
            if (scale != 0.0F) local->scale += inertializer->scale[i].direction * scale;
        }
    }

    /*
     * History, after the offset has been applied, because the velocity a later transition has to match
     * is the one that was *on screen* — not the one the destination clip was producing underneath.
     * That is what makes a second transition during a first one compose rather than fight it.
     */
    inertializer->before_previous  = inertializer->previous;
    inertializer->previous         = *pose;
    inertializer->previous_delta_s = delta_time_s;

    if (inertializer->history_frames < 2) inertializer->history_frames++;
}

b8 nya_skeleton_inertializer_active(const NYA_SkeletonInertializer* inertializer) {
    return inertializer != nullptr && inertializer->elapsed_s < inertializer->longest_s;
}
