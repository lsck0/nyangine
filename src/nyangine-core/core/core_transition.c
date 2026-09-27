#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The one running transition. Zeroed is idle: NYA_TRANSITION_NONE and `active` false. */
typedef struct {
    b8                 active;
    NYA_TransitionKind kind;
    NYA_EaseType       ease;
    NYA_Color          color;
    f32                elapsed_s;
    f32                duration_s;
} _NYA_TransitionState;

NYA_INTERNAL _NYA_TransitionState _nya_transition = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_transition_begin(NYA_TransitionKind kind, f32 duration_s, NYA_Color color, NYA_EaseType ease) {
    nya_assert(kind > NYA_TRANSITION_NONE && kind < NYA_TRANSITION_COUNT, "transition kind %d is not one", (s32)kind);

    // A zero-length transition is an instant switch: nothing to draw, so nothing runs.
    if (duration_s <= 0.0F) {
        _nya_transition.active = false;
        return;
    }

    _nya_transition = (_NYA_TransitionState){
        .active     = true,
        .kind       = kind,
        .ease       = ease,
        .color      = color,
        .elapsed_s  = 0.0F,
        .duration_s = duration_s,
    };
}

void nya_transition_end(void) { _nya_transition.active = false; }

b8 nya_transition_active(void) { return _nya_transition.active; }

f32 nya_transition_alpha(void) {
    if (!_nya_transition.active) return 0.0F;

    f32 t = nya_clamp(_nya_transition.elapsed_s / _nya_transition.duration_s, 0.0F, 1.0F);
    return nya_ease(_nya_transition.ease, t);
}

NYA_TransitionKind nya_transition_kind(void) { return _nya_transition.active ? _nya_transition.kind : NYA_TRANSITION_NONE; }

NYA_Color nya_transition_color(void) { return _nya_transition.color; }

void nya_system_transition_update(f32 delta_time_s) {
    if (!_nya_transition.active) return;

    _nya_transition.elapsed_s += delta_time_s;
    if (_nya_transition.elapsed_s >= _nya_transition.duration_s) _nya_transition.active = false;
}

void nya_transition_draw(NYA_Window* window) {
    if (!_nya_transition.active || window == nullptr) return;

    NYA_TransitionKind kind     = _nya_transition.kind;
    f32                progress = nya_transition_alpha();
    f32                covered  = (kind == NYA_TRANSITION_FADE_IN || kind == NYA_TRANSITION_WIPE_IN) ? 1.0F - progress : progress;
    f32                width    = (f32)window->screen_width;
    f32                height   = (f32)window->screen_height;
    NYA_Color          color    = _nya_transition.color;

    // one rectangle over the frame the layers drew: a wipe covers by area, a fade by alpha.
    switch (kind) {
        case NYA_TRANSITION_WIPE_OUT:
        case NYA_TRANSITION_WIPE_IN:  nya_render2d_rect(window, 0.0F, 0.0F, width * covered, height, color); return;
        case NYA_TRANSITION_FADE_OUT:
        case NYA_TRANSITION_FADE_IN:  color.a *= covered; nya_render2d_rect(window, 0.0F, 0.0F, width, height, color); return;
        case NYA_TRANSITION_NONE:
        case NYA_TRANSITION_COUNT:
        default:                      nya_unreachable();
    }
}
