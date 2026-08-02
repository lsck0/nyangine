#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How close |sin(pitch)| may get to 1 before to_euler switches to its gimbal lock branch. */
#define _NYA_QUATERNION_GIMBAL_LOCK_THRESHOLD 0.99999F

NYA_INTERNAL f32 _nya_quaternion_vector_length(f32x3 vector);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * CONSTRUCTION
 * ─────────────────────────────────────────────────────────
 */

NYA_Quaternion nya_quaternion_create(f32 x, f32 y, f32 z, f32 w) {
    return (NYA_Quaternion){ .x = x, .y = y, .z = z, .w = w };
}

NYA_Quaternion nya_quaternion_from_axis_angle(f32x3 axis, f32 radians) {
    f32 length = _nya_quaternion_vector_length(axis);
    if (length < NYA_EPSILON) return nya_quaternion_identity;

    f32x3 unit = axis / length;

    f32 half = radians * 0.5F;
    f32 sine = sinf(half);

    return (NYA_Quaternion){
        .x = unit.x * sine,
        .y = unit.y * sine,
        .z = unit.z * sine,
        .w = cosf(half),
    };
}

NYA_Quaternion nya_quaternion_from_euler(f32 pitch, f32 yaw, f32 roll) {
    // Half angles: a quaternion encodes a rotation of θ as a rotation of θ/2 in its components.
    f32 sp = sinf(pitch * 0.5F);
    f32 cp = cosf(pitch * 0.5F);
    f32 sy = sinf(yaw * 0.5F);
    f32 cy = cosf(yaw * 0.5F);
    f32 sr = sinf(roll * 0.5F);
    f32 cr = cosf(roll * 0.5F);

    // Expansion of qYaw * qPitch * qRoll, written out rather than composed with three calls to
    // nya_quaternion_multiply because most of that product is multiplication by zero.
    return (NYA_Quaternion){
        .x = cy * sp * cr + sy * cp * sr,
        .y = sy * cp * cr - cy * sp * sr,
        .z = cy * cp * sr - sy * sp * cr,
        .w = cy * cp * cr + sy * sp * sr,
    };
}

void nya_quaternion_to_euler(NYA_Quaternion quaternion, OUT f32* out_pitch, OUT f32* out_yaw, OUT f32* out_roll) {
    NYA_Quaternion q = nya_quaternion_normalize(quaternion);

    // sin(pitch) read off the rotation matrix entry that depends on pitch alone.
    f32 sine_pitch = 2.0F * (q.w * q.x - q.y * q.z);
    sine_pitch     = nya_clamp(sine_pitch, -1.0F, 1.0F);

    f32 pitch = asinf(sine_pitch);
    f32 yaw   = 0.0F;
    f32 roll  = 0.0F;

    if (fabsf(sine_pitch) > _NYA_QUATERNION_GIMBAL_LOCK_THRESHOLD) {
        // Straight up or straight down. Yaw and roll turn about the same world axis here, so only
        // their sum is recoverable; attributing all of it to yaw is the conventional choice and
        // keeps a camera's roll from flipping as it passes the pole.
        yaw  = atan2f(2.0F * (q.w * q.y - q.x * q.z), 1.0F - 2.0F * (q.y * q.y + q.z * q.z));
        roll = 0.0F;
    } else {
        yaw  = atan2f(2.0F * (q.x * q.z + q.w * q.y), 1.0F - 2.0F * (q.x * q.x + q.y * q.y));
        roll = atan2f(2.0F * (q.x * q.y + q.w * q.z), 1.0F - 2.0F * (q.x * q.x + q.z * q.z));
    }

    if (out_pitch) *out_pitch = pitch;
    if (out_yaw) *out_yaw = yaw;
    if (out_roll) *out_roll = roll;
}

void nya_quaternion_to_axis_angle(NYA_Quaternion quaternion, OUT f32x3* out_axis, OUT f32* out_radians) {
    NYA_Quaternion q = nya_quaternion_normalize(quaternion);

    // q and -q are the same rotation; flipping to w >= 0 keeps the reported angle in [0, π].
    if (q.w < 0.0F) q = nya_quaternion_scale(q, -1.0F);

    f32x3 vector      = { q.x, q.y, q.z };
    f32   sine_length = _nya_quaternion_vector_length(vector);

    if (sine_length < NYA_EPSILON) {
        // No rotation, so the axis is arbitrary. X is as good as any and beats handing back a NaN.
        if (out_axis) *out_axis = f32x3_unit_x;
        if (out_radians) *out_radians = 0.0F;
        return;
    }

    if (out_axis) *out_axis = vector / sine_length;
    if (out_radians) *out_radians = 2.0F * atan2f(sine_length, q.w);
}

NYA_Quaternion nya_quaternion_from_to(f32x3 from, f32x3 to) {
    f32 from_length = _nya_quaternion_vector_length(from);
    f32 to_length   = _nya_quaternion_vector_length(to);
    if (from_length < NYA_EPSILON || to_length < NYA_EPSILON) return nya_quaternion_identity;

    f32x3 a = from / from_length;
    f32x3 b = to / to_length;

    f32x3 product = a * b;
    f32   cosine  = product.x + product.y + product.z;

    if (cosine > 1.0F - NYA_EPSILON) return nya_quaternion_identity;

    if (cosine < -1.0F + NYA_EPSILON) {
        // Exactly opposite: every axis perpendicular to `a` is a valid half turn, and the cross
        // product below would be zero. Pick whichever cardinal axis is least parallel to `a`.
        f32x3 fallback = fabsf(a.x) < 0.9F ? f32x3_unit_x : f32x3_unit_y;

        f32x3 axis = {
            a.y * fallback.z - a.z * fallback.y,
            a.z * fallback.x - a.x * fallback.z,
            a.x * fallback.y - a.y * fallback.x,
        };

        return nya_quaternion_from_axis_angle(axis, (f32)M_PI);
    }

    f32x3 axis = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };

    // w = 1 + cos θ together with the unnormalized cross product gives the half angle rotation
    // directly, so no trigonometry is needed.
    NYA_Quaternion result = { .x = axis.x, .y = axis.y, .z = axis.z, .w = 1.0F + cosine };
    return nya_quaternion_normalize(result);
}

NYA_Quaternion nya_quaternion_look(f32x3 direction, f32x3 up) {
    f32 direction_length = _nya_quaternion_vector_length(direction);
    if (direction_length < NYA_EPSILON) return nya_quaternion_identity;

    f32x3 forward = direction / direction_length;

    // -Z is forward, matching the convention the view matrix uses.
    f32x3          backward = -forward;
    NYA_Quaternion swing    = nya_quaternion_from_to(-f32x3_unit_z, forward);

    // Twist about the new forward axis until Y lines up with `up` as closely as it can.
    f32x3 right = {
        up.y * backward.z - up.z * backward.y,
        up.z * backward.x - up.x * backward.z,
        up.x * backward.y - up.y * backward.x,
    };

    f32 right_length = _nya_quaternion_vector_length(right);
    if (right_length < NYA_EPSILON) return swing; // looking straight along `up`, roll is arbitrary

    right = right / right_length;

    f32x3 desired_up = {
        backward.y * right.z - backward.z * right.y,
        backward.z * right.x - backward.x * right.z,
        backward.x * right.y - backward.y * right.x,
    };

    f32x3 swung_up = nya_quaternion_rotate(swing, f32x3_unit_y);
    return nya_quaternion_multiply(nya_quaternion_from_to(swung_up, desired_up), swing);
}

/*
 * ─────────────────────────────────────────────────────────
 * ALGEBRA
 * ─────────────────────────────────────────────────────────
 */

NYA_Quaternion nya_quaternion_multiply(NYA_Quaternion a, NYA_Quaternion b) {
    return (NYA_Quaternion){
        .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

NYA_Quaternion nya_quaternion_add(NYA_Quaternion a, NYA_Quaternion b) {
    return (NYA_Quaternion){ .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z, .w = a.w + b.w };
}

NYA_Quaternion nya_quaternion_scale(NYA_Quaternion quaternion, f32 scalar) {
    return (NYA_Quaternion){
        .x = quaternion.x * scalar,
        .y = quaternion.y * scalar,
        .z = quaternion.z * scalar,
        .w = quaternion.w * scalar,
    };
}

NYA_Quaternion nya_quaternion_conjugate(NYA_Quaternion quaternion) {
    return (NYA_Quaternion){ .x = -quaternion.x, .y = -quaternion.y, .z = -quaternion.z, .w = quaternion.w };
}

NYA_Quaternion nya_quaternion_inverse(NYA_Quaternion quaternion) {
    f32 length_squared = nya_quaternion_length_squared(quaternion);
    if (length_squared < NYA_EPSILON) return nya_quaternion_identity;

    return nya_quaternion_scale(nya_quaternion_conjugate(quaternion), 1.0F / length_squared);
}

f32 nya_quaternion_dot(NYA_Quaternion a, NYA_Quaternion b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

f32 nya_quaternion_length_squared(NYA_Quaternion quaternion) {
    return nya_quaternion_dot(quaternion, quaternion);
}

f32 nya_quaternion_length(NYA_Quaternion quaternion) {
    return sqrtf(nya_quaternion_length_squared(quaternion));
}

NYA_Quaternion nya_quaternion_normalize(NYA_Quaternion quaternion) {
    f32 length = nya_quaternion_length(quaternion);
    if (length < NYA_EPSILON) return nya_quaternion_identity;

    return nya_quaternion_scale(quaternion, 1.0F / length);
}

f32 nya_quaternion_angle_between(NYA_Quaternion a, NYA_Quaternion b) {
    NYA_Quaternion left  = nya_quaternion_normalize(a);
    NYA_Quaternion right = nya_quaternion_normalize(b);

    // fabsf collapses q and -q, which are the same rotation, so the answer never exceeds π.
    f32 cosine = fabsf(nya_quaternion_dot(left, right));
    cosine     = nya_clamp(cosine, -1.0F, 1.0F);

    return 2.0F * acosf(cosine);
}

b8 nya_quaternion_approx_equals(NYA_Quaternion a, NYA_Quaternion b, f32 epsilon) {
    return fabsf(nya_quaternion_dot(nya_quaternion_normalize(a), nya_quaternion_normalize(b))) >= 1.0F - epsilon;
}

/*
 * ─────────────────────────────────────────────────────────
 * APPLICATION
 * ─────────────────────────────────────────────────────────
 */

f32x3 nya_quaternion_rotate(NYA_Quaternion quaternion, f32x3 vector) {
    f32x3 axis = { quaternion.x, quaternion.y, quaternion.z };

    // v + 2 * (axis × (axis × v + w * v)). Equivalent to q * v * q⁻¹ but without building the
    // intermediate quaternions.
    f32x3 inner = {
        axis.y * vector.z - axis.z * vector.y + quaternion.w * vector.x,
        axis.z * vector.x - axis.x * vector.z + quaternion.w * vector.y,
        axis.x * vector.y - axis.y * vector.x + quaternion.w * vector.z,
    };

    f32x3 outer = {
        axis.y * inner.z - axis.z * inner.y,
        axis.z * inner.x - axis.x * inner.z,
        axis.x * inner.y - axis.y * inner.x,
    };

    return vector + outer * 2.0F;
}

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATION
 * ─────────────────────────────────────────────────────────
 */

NYA_Quaternion nya_quaternion_nlerp(NYA_Quaternion a, NYA_Quaternion b, f32 t) {
    // Flip b onto the same hemisphere as a, otherwise the interpolation takes the long way round.
    NYA_Quaternion target = nya_quaternion_dot(a, b) < 0.0F ? nya_quaternion_scale(b, -1.0F) : b;

    NYA_Quaternion blended = nya_quaternion_add(nya_quaternion_scale(a, 1.0F - t), nya_quaternion_scale(target, t));
    return nya_quaternion_normalize(blended);
}

NYA_Quaternion nya_quaternion_slerp(NYA_Quaternion a, NYA_Quaternion b, f32 t) {
    NYA_Quaternion start = nya_quaternion_normalize(a);
    NYA_Quaternion end   = nya_quaternion_normalize(b);

    f32 cosine = nya_quaternion_dot(start, end);

    if (cosine < 0.0F) {
        end    = nya_quaternion_scale(end, -1.0F);
        cosine = -cosine;
    }

    // Nearly parallel: sin θ approaches zero and the division below loses all its precision. nlerp
    // and slerp agree to well under a float's worth of error at this angle anyway.
    if (cosine > 1.0F - NYA_EPSILON) return nya_quaternion_nlerp(start, end, t);

    f32 theta = acosf(nya_clamp(cosine, -1.0F, 1.0F));
    f32 sine  = sinf(theta);

    f32 start_weight = sinf((1.0F - t) * theta) / sine;
    f32 end_weight   = sinf(t * theta) / sine;

    return nya_quaternion_add(nya_quaternion_scale(start, start_weight), nya_quaternion_scale(end, end_weight));
}

/*
 * ─────────────────────────────────────────────────────────
 * MATRIX CONVERSION
 * ─────────────────────────────────────────────────────────
 */

f32_3x3 nya_quaternion_to_matrix3(NYA_Quaternion quaternion) {
    NYA_Quaternion q = nya_quaternion_normalize(quaternion);

    f32 xx = q.x * q.x;
    f32 yy = q.y * q.y;
    f32 zz = q.z * q.z;
    f32 xy = q.x * q.y;
    f32 xz = q.x * q.z;
    f32 yz = q.y * q.z;
    f32 wx = q.w * q.x;
    f32 wy = q.w * q.y;
    f32 wz = q.w * q.z;

    return nya_matrix_create(
        (f32x3){ 1.0F - 2.0F * (yy + zz), 2.0F * (xy - wz), 2.0F * (xz + wy) },
        (f32x3){ 2.0F * (xy + wz), 1.0F - 2.0F * (xx + zz), 2.0F * (yz - wx) },
        (f32x3){ 2.0F * (xz - wy), 2.0F * (yz + wx), 1.0F - 2.0F * (xx + yy) }
    );
}

f32_4x4 nya_quaternion_to_matrix4(NYA_Quaternion quaternion) {
    f32_3x3 rotation = nya_quaternion_to_matrix3(quaternion);

    return nya_matrix_create(
        (f32x4){ rotation[0][0], rotation[0][1], rotation[0][2], 0.0F },
        (f32x4){ rotation[1][0], rotation[1][1], rotation[1][2], 0.0F },
        (f32x4){ rotation[2][0], rotation[2][1], rotation[2][2], 0.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

NYA_Quaternion nya_quaternion_from_matrix(f32_3x3 matrix) {
    f32 trace = matrix[0][0] + matrix[1][1] + matrix[2][2];

    // Shepperd's method. Each branch divides by a component known to be large in that case, which
    // is what keeps the result accurate; the naive single formula loses precision near a half turn.
    if (trace > 0.0F) {
        f32 scale = sqrtf(trace + 1.0F) * 2.0F;
        return nya_quaternion_normalize((NYA_Quaternion){
            .x = (matrix[2][1] - matrix[1][2]) / scale,
            .y = (matrix[0][2] - matrix[2][0]) / scale,
            .z = (matrix[1][0] - matrix[0][1]) / scale,
            .w = 0.25F * scale,
        });
    }

    if (matrix[0][0] > matrix[1][1] && matrix[0][0] > matrix[2][2]) {
        f32 scale = sqrtf(1.0F + matrix[0][0] - matrix[1][1] - matrix[2][2]) * 2.0F;
        return nya_quaternion_normalize((NYA_Quaternion){
            .x = 0.25F * scale,
            .y = (matrix[0][1] + matrix[1][0]) / scale,
            .z = (matrix[0][2] + matrix[2][0]) / scale,
            .w = (matrix[2][1] - matrix[1][2]) / scale,
        });
    }

    if (matrix[1][1] > matrix[2][2]) {
        f32 scale = sqrtf(1.0F + matrix[1][1] - matrix[0][0] - matrix[2][2]) * 2.0F;
        return nya_quaternion_normalize((NYA_Quaternion){
            .x = (matrix[0][1] + matrix[1][0]) / scale,
            .y = 0.25F * scale,
            .z = (matrix[1][2] + matrix[2][1]) / scale,
            .w = (matrix[0][2] - matrix[2][0]) / scale,
        });
    }

    f32 scale = sqrtf(1.0F + matrix[2][2] - matrix[0][0] - matrix[1][1]) * 2.0F;
    return nya_quaternion_normalize((NYA_Quaternion){
        .x = (matrix[0][2] + matrix[2][0]) / scale,
        .y = (matrix[1][2] + matrix[2][1]) / scale,
        .z = 0.25F * scale,
        .w = (matrix[1][0] - matrix[0][1]) / scale,
    });
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_quaternion_vector_length(f32x3 vector) {
    f32x3 squared = vector * vector;
    return sqrtf(squared.x + squared.y + squared.z);
}
