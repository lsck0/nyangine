#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_vector_dot(f32x2 a, f32x2 b) __attr_overloaded {
    f32x2 product = a * b;

    return product.x + product.y;
}

f32 nya_vector_dot(f32x3 a, f32x3 b) __attr_overloaded {
    f32x3 product = a * b;

    return product.x + product.y + product.z;
}

f32x3 nya_vector_cross(f32x3 a, f32x3 b) {
    // Written out rather than built from two shuffles and a subtract. The shuffle form is shorter and
    // is the one that gets the lane order wrong silently; this one is checkable against the
    // definition by eye.
    return (f32x3){
        (a.y * b.z) - (a.z * b.y),
        (a.z * b.x) - (a.x * b.z),
        (a.x * b.y) - (a.y * b.x),
    };
}

f32 nya_vector_length(f32x2 vector) __attr_overloaded {
    return sqrtf(nya_vector_dot(vector, vector));
}

f32 nya_vector_length(f32x3 vector) __attr_overloaded {
    return sqrtf(nya_vector_dot(vector, vector));
}

f32x2 nya_vector_normalize(f32x2 vector) __attr_overloaded {
    f32 length = nya_vector_length(vector);

    // Tested against the epsilon rather than against zero: a vector short enough that its reciprocal
    // overflows is not usefully different from zero, and dividing by it produces infinities that
    // survive every later multiply.
    if (length < NYA_EPSILON) return f32x2_zero;

    return vector / length;
}

f32x3 nya_vector_normalize(f32x3 vector) __attr_overloaded {
    f32 length = nya_vector_length(vector);

    if (length < NYA_EPSILON) return f32x3_zero;

    return vector / length;
}
