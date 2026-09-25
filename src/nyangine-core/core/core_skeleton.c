#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One bone's local transform as a matrix. Scale, then rotate, then translate. */
NYA_INTERNAL f32_4x4 _nya_skeleton_transform_matrix(NYA_BoneTransform transform);

/** Blends two bone transforms. Rotation by slerp, the rest linearly. See NYA_BoneTransform. */
NYA_INTERNAL NYA_BoneTransform _nya_skeleton_transform_blend(NYA_BoneTransform a, NYA_BoneTransform b, f32 amount);

/**
 * Which two baked frames `time_s` falls between, and how far.
 * */
NYA_INTERNAL void _nya_skeleton_frame_pair(const NYA_SkeletonClip* clip, f32 time_s, OUT u32* out_frame, OUT u32* out_next, OUT f32* out_blend);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const NYA_SkeletonClip* nya_skeleton_clip(const NYA_Skeleton* skeleton, NYA_ConstCString name) {
    if (skeleton == nullptr || name == nullptr) return nullptr;

    for (u32 i = 0; i < skeleton->clip_count; i++) {
        if (nya_string_equals(skeleton->clips[i].name, name)) return &skeleton->clips[i];
    }

    return nullptr;
}

s32 nya_skeleton_bone_index(const NYA_Skeleton* skeleton, NYA_ConstCString name) {
    if (skeleton == nullptr || name == nullptr) return -1;

    for (u32 i = 0; i < skeleton->bone_count; i++) {
        if (nya_string_equals(skeleton->bones[i].name, name)) return (s32)i;
    }

    return -1;
}

void nya_skeleton_pose_rest(const NYA_Skeleton* skeleton, OUT NYA_SkeletonPose* out_pose) {
    nya_assert(skeleton != nullptr);
    nya_assert(out_pose != nullptr);
    nya_assert(skeleton->bone_count <= NYA_SKELETON_MAX_BONES, "the importer caps a skeleton at NYA_SKELETON_MAX_BONES");

    out_pose->bone_count = skeleton->bone_count;

    for (u32 i = 0; i < skeleton->bone_count; i++) {
        out_pose->local[i] = skeleton->bones[i].rest;
    }
}

void nya_skeleton_pose_sample(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, f32 time_s,
                             OUT NYA_SkeletonPose* out_pose) {
    if (skeleton == nullptr || out_pose == nullptr) return;

    // no clip, or an empty one, gives the rest pose so a character whose animation failed to load still
    // stands.
    if (clip == nullptr || clip->frame_count == 0 || clip->frames == nullptr) {
        nya_skeleton_pose_rest(skeleton, out_pose);
        return;
    }

    u32 bone_count = skeleton->bone_count < NYA_SKELETON_MAX_BONES ? skeleton->bone_count : NYA_SKELETON_MAX_BONES;

    out_pose->bone_count = bone_count;

    u32 frame = 0;
    u32 next  = 0;
    f32 blend = 0.0F;
    _nya_skeleton_frame_pair(clip, time_s, &frame, &next, &blend);

    const NYA_BoneTransform* current = &clip->frames[(u64)frame * skeleton->bone_count];
    const NYA_BoneTransform* upcoming = &clip->frames[(u64)next * skeleton->bone_count];

    for (u32 i = 0; i < bone_count; i++) {
        out_pose->local[i] = _nya_skeleton_transform_blend(current[i], upcoming[i], blend);
    }
}

NYA_BoneTransform nya_skeleton_clip_bone(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, s32 bone, f32 time_s) {
    if (skeleton == nullptr || bone < 0 || (u32)bone >= skeleton->bone_count) {
        return (NYA_BoneTransform){ .rotation = nya_quaternion_identity, .scale = { 1.0F, 1.0F, 1.0F } };
    }

    if (clip == nullptr || clip->frame_count == 0 || clip->frames == nullptr) return skeleton->bones[bone].rest;

    u32 frame = 0;
    u32 next  = 0;
    f32 blend = 0.0F;
    _nya_skeleton_frame_pair(clip, time_s, &frame, &next, &blend);

    return _nya_skeleton_transform_blend(clip->frames[((u64)frame * skeleton->bone_count) + (u64)bone],
                                         clip->frames[((u64)next * skeleton->bone_count) + (u64)bone], blend);
}

void nya_skeleton_pose_blend(const NYA_SkeletonPose* from, const NYA_SkeletonPose* to, f32 amount,
                            OUT NYA_SkeletonPose* out_pose) {
    if (from == nullptr || to == nullptr || out_pose == nullptr) return;

    u32 bone_count = from->bone_count < to->bone_count ? from->bone_count : to->bone_count;

    out_pose->bone_count = bone_count;

    for (u32 i = 0; i < bone_count; i++) {
        out_pose->local[i] = _nya_skeleton_transform_blend(from->local[i], to->local[i], amount);
    }
}

void nya_skeleton_animator_play(NYA_SkeletonAnimator* animator, const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip,
                               b8 looping) {
    if (animator == nullptr) return;

    *animator = (NYA_SkeletonAnimator){
        .skeleton = skeleton,
        .clip     = clip,
        .time_s   = 0.0F,
        .speed    = 1.0F,
        .playing  = clip != nullptr,
        .looping  = looping,
    };
}

void nya_skeleton_animator_update(NYA_SkeletonAnimator* animator, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose) {
    nya_trace_scope(NYA_TRACE_SKINNING);

    if (animator == nullptr) return;
    if (animator->skeleton == nullptr) return;

    if (animator->clip == nullptr) {
        if (out_pose != nullptr) nya_skeleton_pose_rest(animator->skeleton, out_pose);
        return;
    }

    animator->time_previous_s = animator->time_s;

    if (animator->playing) animator->time_s += delta_time_s * animator->speed;

    f32 duration = animator->clip->duration_s;

    if (duration > 0.0F) {
        if (animator->looping) {
            /*
             * Wrapped with fmod so a long pause or a large speed cannot leave the clock outside the
             * clip. The negative case is added back rather than clamped, which is what makes playing
             * backward loop instead of sticking at zero.
             */
            animator->time_s = fmodf(animator->time_s, duration);

            if (animator->time_s < 0.0F) animator->time_s += duration;
        } else if (animator->time_s >= duration) {
            animator->time_s  = duration;
            animator->playing = false;

            // Latched rather than momentary: a game polls this, and a flag true for one frame is a
            // flag whoever checks on the next frame will miss.
            animator->finished = true;
        } else if (animator->time_s < 0.0F) {
            animator->time_s   = 0.0F;
            animator->playing  = false;
            animator->finished = true;
        }
    }

    if (out_pose != nullptr) nya_skeleton_pose_sample(animator->skeleton, animator->clip, animator->time_s, out_pose);
}

void nya_skeleton_animator_render_pose(const NYA_SkeletonAnimator* animator, OUT NYA_SkeletonPose* out_pose) {
    nya_trace_scope(NYA_TRACE_SKINNING);

    if (animator == nullptr || out_pose == nullptr) return;
    if (animator->skeleton == nullptr) return;

    // sampled rather than blended from two stored poses: as cheap, exact, and nothing extra kept per animator.
    f32 duration = animator->clip != nullptr ? animator->clip->duration_s : 0.0F;
    f32 step     = animator->time_s - animator->time_previous_s;

    // a looping clock that wrapped this tick moved a little forward, not back across the clip.
    if (animator->looping && duration > 0.0F) {
        if (step > duration * 0.5F) step -= duration;
        if (step < -duration * 0.5F) step += duration;
    }

    f32 time_s = animator->time_previous_s + (step * nya_app_tick_alpha());

    if (animator->looping && duration > 0.0F) {
        time_s = fmodf(time_s, duration);
        if (time_s < 0.0F) time_s += duration;
    }

    // a null clip samples the rest pose.
    nya_skeleton_pose_sample(animator->skeleton, animator->clip, time_s, out_pose);
}

void nya_skeleton_model_transforms(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_model) {
    if (skeleton == nullptr || pose == nullptr || out_model == nullptr) return;

    u32 bone_count = skeleton->bone_count < NYA_SKELETON_MAX_BONES ? skeleton->bone_count : NYA_SKELETON_MAX_BONES;

    /*
     * One forward pass, correct because the importer orders bones parent first (see
     * _nya_asset_mesh_skeleton). The index check makes a file breaking that order fail loudly instead of
     * reading an unwritten entry.
     */
    for (u32 i = 0; i < bone_count; i++) {
        f32_4x4 local = _nya_skeleton_transform_matrix(pose->local[i]);

        s32 parent = skeleton->bones[i].parent;

        out_model[i] = parent >= 0 && (u32)parent < i ? out_model[parent] * local : local;
    }
}

void nya_skeleton_palette(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_palette) {
    nya_trace_scope(NYA_TRACE_SKINNING);

    if (skeleton == nullptr || pose == nullptr || out_palette == nullptr) return;

    u32 bone_count = skeleton->bone_count < NYA_SKELETON_MAX_BONES ? skeleton->bone_count : NYA_SKELETON_MAX_BONES;

    // written into the caller's array and folded in place: bone i's model transform is not needed once
    // its palette entry is built, so no scratch.
    nya_skeleton_model_transforms(skeleton, pose, out_palette);

    for (u32 i = 0; i < bone_count; i++) {
        // The bind pose undone, then the animated pose applied. A bone that has not moved leaves the
        // vertex exactly where it was authored, which is the identity this must produce.
        out_palette[i] = out_palette[i] * skeleton->bones[i].inverse_bind;
    }
}

b8 nya_skeleton_bone_model(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, s32 bone, OUT f32_4x4* out_transform) {
    if (skeleton == nullptr || pose == nullptr || out_transform == nullptr) return false;
    if (bone < 0 || (u32)bone >= skeleton->bone_count || bone >= NYA_SKELETON_MAX_BONES) return false;

    /*
     * Only this bone's chain. A socket asks about a hand or a head, and composing depth transforms beats
     * computing every bone in the rig.
     */
    s32 chain[NYA_SKELETON_MAX_BONES];
    u32 depth = 0;

    for (s32 at = bone; at >= 0 && depth < NYA_SKELETON_MAX_BONES; at = skeleton->bones[at].parent) {
        nya_assert(skeleton->bones[at].parent < at, "bone %d is not ordered after its parent", at);
        chain[depth++] = at;
    }

    nya_assert(depth > 0, "a bone that resolved has at least itself in its parent chain");

    f32_4x4 model = _nya_skeleton_transform_matrix(pose->local[chain[depth - 1]]);

    // Root first, which is the order the chain was collected in reverse.
    for (u32 i = depth - 1; i > 0; i--) model = model * _nya_skeleton_transform_matrix(pose->local[chain[i - 1]]);

    *out_transform = model;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32_4x4 _nya_skeleton_transform_matrix(NYA_BoneTransform transform) {
    f32_4x4 rotation = nya_quaternion_to_matrix4(transform.rotation);

    // Scale folded into the rotation's columns, which is the same as rotation * scale without
    // building a second matrix to multiply by.
    for (u32 row = 0; row < 3; row++) {
        rotation[row][0] *= transform.scale.x;
        rotation[row][1] *= transform.scale.y;
        rotation[row][2] *= transform.scale.z;
    }

    rotation[0][3] = transform.translation.x;
    rotation[1][3] = transform.translation.y;
    rotation[2][3] = transform.translation.z;

    return rotation;
}

void _nya_skeleton_frame_pair(const NYA_SkeletonClip* clip, f32 time_s, OUT u32* out_frame, OUT u32* out_next, OUT f32* out_blend) {
    nya_assert(clip->frame_count > 0);

    // clamped, not wrapped: looping belongs to the animator.
    f32 clamped = time_s;
    if (clamped < 0.0F) clamped = 0.0F;
    if (clamped > clip->duration_s) clamped = clip->duration_s;

    f32 position = clamped * clip->frame_rate;

    u32 frame = (u32)position;
    if (frame >= clip->frame_count - 1) frame = clip->frame_count > 1 ? clip->frame_count - 2 : 0;

    f32 blend = position - (f32)frame;
    if (blend < 0.0F) blend = 0.0F;
    if (blend > 1.0F) blend = 1.0F;

    *out_frame = frame;
    *out_next  = frame + 1 < clip->frame_count ? frame + 1 : frame;
    *out_blend = blend;
}

NYA_BoneTransform _nya_skeleton_transform_blend(NYA_BoneTransform a, NYA_BoneTransform b, f32 amount) {
    if (amount <= 0.0F) return a;
    if (amount >= 1.0F) return b;

    return (NYA_BoneTransform){
        .translation = a.translation + ((b.translation - a.translation) * amount),

        // Slerp rather than a component lerp: two rotations lerped component-wise pass *through* the
        // interior of the sphere, which shortens the rotation and makes a limb visibly shrink mid swing.
        .rotation = nya_quaternion_slerp_unit(a.rotation, b.rotation, amount),

        .scale = a.scale + ((b.scale - a.scale) * amount),
    };
}
