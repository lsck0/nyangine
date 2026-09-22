/**
 * @file base_compare.h
 *
 * nya_min and nya_max, which the containers need to grow and which are therefore in base rather than in
 * math, where the rest of the scalar helpers live. math_scalar.h includes this, so a caller of either finds
 * them there as before.
 *
 * Both bind their arguments to temporaries, so an argument is evaluated once, and refuse two operands of
 * different types, since a mixed comparison converts one of them in a direction nobody chose.
 * */
#pragma once

#include "nyangine/base/base_assert.h"

#define nya_min(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __auto_type _nya_min_a = (a);                                                                                                                \
        __auto_type _nya_min_b = (b);                                                                                                                \
        nya_assert_type_match(_nya_min_a, _nya_min_b);                                                                                               \
        _nya_min_a < _nya_min_b ? _nya_min_a : _nya_min_b;                                                                                           \
    })

#define nya_max(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __auto_type _nya_max_a = (a);                                                                                                                \
        __auto_type _nya_max_b = (b);                                                                                                                \
        nya_assert_type_match(_nya_max_a, _nya_max_b);                                                                                               \
        _nya_max_a > _nya_max_b ? _nya_max_a : _nya_max_b;                                                                                           \
    })
