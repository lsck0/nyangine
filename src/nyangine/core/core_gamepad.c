#include "nyangine/nyangine.h"

#ifndef NYA_NO_SDL
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_init.h"
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    b8            open;
    NYA_GamepadId id;
    SDL_Gamepad*  handle;

    b8 pressed[NYA_GAMEPAD_BUTTON_COUNT];
    b8 just_pressed[NYA_GAMEPAD_BUTTON_COUNT];
    b8 just_released[NYA_GAMEPAD_BUTTON_COUNT];

    /** Raw, still in SDL's [-32768, 32767]. Deadzoning happens on read so the constants stay tunable. */
    s16 axes[NYA_GAMEPAD_AXIS_COUNT];

    /** Last frame's, so an axis used as a button has edges the way a real button does. */
    s16 axes_previous[NYA_GAMEPAD_AXIS_COUNT];

    NYA_GamepadKind kind;
    b8              has_rumble;
} _NYA_GamepadSlot;

typedef struct {
    _NYA_GamepadSlot slots[NYA_GAMEPAD_MAX];
    b8               ready;
} _NYA_GamepadSystem;

NYA_INTERNAL _NYA_GamepadSystem _nya_gamepad_system = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL _NYA_GamepadSlot* _nya_gamepad_slot(NYA_GamepadId pad) {
    if (pad == NYA_GAMEPAD_NONE) return nullptr;

    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        if (_nya_gamepad_system.slots[i].open && _nya_gamepad_system.slots[i].id == pad) return &_nya_gamepad_system.slots[i];
    }

    return nullptr;
}

#ifndef NYA_NO_SDL

/** SDL's button enum onto ours. Anything we do not name is dropped rather than mapped to a neighbour. */
NYA_INTERNAL b8 _nya_gamepad_button_from_sdl(u8 sdl_button, OUT NYA_GamepadButton* out) {
    switch ((SDL_GamepadButton)sdl_button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: *out = NYA_GAMEPAD_BUTTON_SOUTH; return true;
        case SDL_GAMEPAD_BUTTON_EAST: *out = NYA_GAMEPAD_BUTTON_EAST; return true;
        case SDL_GAMEPAD_BUTTON_WEST: *out = NYA_GAMEPAD_BUTTON_WEST; return true;
        case SDL_GAMEPAD_BUTTON_NORTH: *out = NYA_GAMEPAD_BUTTON_NORTH; return true;
        case SDL_GAMEPAD_BUTTON_BACK: *out = NYA_GAMEPAD_BUTTON_BACK; return true;
        case SDL_GAMEPAD_BUTTON_GUIDE: *out = NYA_GAMEPAD_BUTTON_GUIDE; return true;
        case SDL_GAMEPAD_BUTTON_START: *out = NYA_GAMEPAD_BUTTON_START; return true;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: *out = NYA_GAMEPAD_BUTTON_LEFT_STICK; return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: *out = NYA_GAMEPAD_BUTTON_RIGHT_STICK; return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: *out = NYA_GAMEPAD_BUTTON_LEFT_SHOULDER; return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: *out = NYA_GAMEPAD_BUTTON_RIGHT_SHOULDER; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: *out = NYA_GAMEPAD_BUTTON_DPAD_UP; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: *out = NYA_GAMEPAD_BUTTON_DPAD_DOWN; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: *out = NYA_GAMEPAD_BUTTON_DPAD_LEFT; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: *out = NYA_GAMEPAD_BUTTON_DPAD_RIGHT; return true;
        default: return false;
    }
}

NYA_INTERNAL b8 _nya_gamepad_axis_from_sdl(u8 sdl_axis, OUT NYA_GamepadAxis* out) {
    switch ((SDL_GamepadAxis)sdl_axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: *out = NYA_GAMEPAD_AXIS_LEFT_X; return true;
        case SDL_GAMEPAD_AXIS_LEFTY: *out = NYA_GAMEPAD_AXIS_LEFT_Y; return true;
        case SDL_GAMEPAD_AXIS_RIGHTX: *out = NYA_GAMEPAD_AXIS_RIGHT_X; return true;
        case SDL_GAMEPAD_AXIS_RIGHTY: *out = NYA_GAMEPAD_AXIS_RIGHT_Y; return true;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: *out = NYA_GAMEPAD_AXIS_LEFT_TRIGGER; return true;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: *out = NYA_GAMEPAD_AXIS_RIGHT_TRIGGER; return true;
        default: return false;
    }
}

NYA_INTERNAL NYA_GamepadKind _nya_gamepad_kind_from_sdl(SDL_Gamepad* handle) {
    switch (SDL_GetGamepadType(handle)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return NYA_GAMEPAD_KIND_XBOX;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return NYA_GAMEPAD_KIND_PLAYSTATION;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return NYA_GAMEPAD_KIND_NINTENDO;
        default: return NYA_GAMEPAD_KIND_UNKNOWN;
    }
}

NYA_INTERNAL void _nya_gamepad_open(SDL_JoystickID which) {
    if (_nya_gamepad_slot((NYA_GamepadId)which) != nullptr) return;

    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        _NYA_GamepadSlot* slot = &_nya_gamepad_system.slots[i];
        if (slot->open) continue;

        SDL_Gamepad* handle = SDL_OpenGamepad(which);
        if (handle == nullptr) {
            nya_log_warn("SDL_OpenGamepad() failed for %u: %s", (u32)which, SDL_GetError());
            return;
        }

        *slot = (_NYA_GamepadSlot){
            .open   = true,
            .id     = (NYA_GamepadId)which,
            .handle = handle,
            .kind   = _nya_gamepad_kind_from_sdl(handle),
        };

        // Probed rather than assumed: rumble support varies by controller and by OS both.
        SDL_PropertiesID properties = SDL_GetGamepadProperties(handle);
        slot->has_rumble            = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);

        nya_log_info("Gamepad connected: '%s' (id %u, %s rumble).", SDL_GetGamepadName(handle), (u32)which,
                     slot->has_rumble ? "has" : "no");
        return;
    }

    nya_log_warn("A gamepad connected but all " FMTu32 " slots are taken; it was ignored.", (u32)NYA_GAMEPAD_MAX);
}

NYA_INTERNAL void _nya_gamepad_close(SDL_JoystickID which) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot((NYA_GamepadId)which);
    if (slot == nullptr) return;

    if (slot->handle != nullptr) SDL_CloseGamepad(slot->handle);

    nya_log_info("Gamepad disconnected: id %u.", (u32)which);

    // Zeroed whole, so a pad reconnecting into this slot cannot inherit the last one's held buttons.
    *slot = (_NYA_GamepadSlot){ 0 };
}

#endif // !NYA_NO_SDL

/** SDL's s16 onto [-1, 1], with a one-sided dead zone rescaled so the live range still reaches 1. */
NYA_INTERNAL f32 _nya_gamepad_normalize(s16 raw, f32 deadzone) {
    f32 value = (f32)raw / 32767.0F;
    value     = nya_clamp(value, -1.0F, 1.0F);

    f32 magnitude = fabsf(value);
    if (magnitude < deadzone) return 0.0F;

    // Rescaled rather than clamped: without this the value jumps from 0 to the dead zone the instant
    // the stick crosses it, which reads as a control that snaps rather than one that eases in.
    f32 scaled = (magnitude - deadzone) / (1.0F - deadzone);

    return value < 0.0F ? -scaled : scaled;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_gamepad_init(void) {
    _nya_gamepad_system = (_NYA_GamepadSystem){ 0 };

#ifndef NYA_NO_SDL
    /*
     * Background events before the subsystem comes up, because the hint is read at init.
     *
     * A pad held while the window is not focused is otherwise silent, which matters for a game being
     * streamed and for anyone playing on a TV with the window not quite focused.
     */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        nya_log_warn("SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed, gamepads are unavailable: %s", SDL_GetError());
        return;
    }

    _nya_gamepad_system.ready = true;

    /*
     * Nothing is enumerated here on purpose.
     *
     * SDL_EVENT_GAMEPAD_ADDED is posted for every already-connected pad as well as for later ones, so
     * opening on that event alone is the whole story. Enumerating here too opens each one twice.
     */
    nya_log_info("Gamepad system initialized (" FMTu32 " slots).", (u32)NYA_GAMEPAD_MAX);
#else
    nya_log_info("Gamepad system initialized (no SDL; gamepads unavailable).");
#endif
}

void nya_system_gamepad_deinit(void) {
#ifndef NYA_NO_SDL
    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        _NYA_GamepadSlot* slot = &_nya_gamepad_system.slots[i];
        if (!slot->open || slot->handle == nullptr) continue;

        // Stopped before closing: a pad left buzzing keeps buzzing after the process is gone.
        SDL_RumbleGamepad(slot->handle, 0, 0, 0);
        SDL_CloseGamepad(slot->handle);
    }

    if (_nya_gamepad_system.ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif

    _nya_gamepad_system = (_NYA_GamepadSystem){ 0 };
    nya_log_info("Gamepad system deinitialized.");
}

void nya_system_gamepad_frame_begin(void) {
    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        _NYA_GamepadSlot* slot = &_nya_gamepad_system.slots[i];
        if (!slot->open) continue;

        nya_memset(slot->just_pressed, 0, sizeof(slot->just_pressed));
        nya_memset(slot->just_released, 0, sizeof(slot->just_released));

        // Snapshot before this frame's motion events arrive, which is what gives an axis-as-button
        // the same just-pressed and just-released edges an ordinary button has.
        nya_memcpy(slot->axes_previous, slot->axes, sizeof(slot->axes));
    }
}

b8 nya_system_gamepad_handle_sdl_event(const void* sdl_event) {
#ifdef NYA_NO_SDL
    nya_unused(sdl_event);
    return false;
#else
    if (sdl_event == nullptr) return false;

    const SDL_Event* event = sdl_event;

    switch (event->type) {
        case SDL_EVENT_GAMEPAD_ADDED: _nya_gamepad_open(event->gdevice.which); return true;
        case SDL_EVENT_GAMEPAD_REMOVED: _nya_gamepad_close(event->gdevice.which); return true;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            _NYA_GamepadSlot* slot = _nya_gamepad_slot((NYA_GamepadId)event->gbutton.which);
            if (slot == nullptr) return true;

            NYA_GamepadButton button = NYA_GAMEPAD_BUTTON_SOUTH;
            if (!_nya_gamepad_button_from_sdl(event->gbutton.button, &button)) return true;

            b8 down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;

            // Guarded, because a repeat down for a button already held is not a new edge.
            if (down && !slot->pressed[button]) slot->just_pressed[button] = true;
            if (!down && slot->pressed[button]) slot->just_released[button] = true;

            slot->pressed[button] = down;
            return true;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            _NYA_GamepadSlot* slot = _nya_gamepad_slot((NYA_GamepadId)event->gaxis.which);
            if (slot == nullptr) return true;

            NYA_GamepadAxis axis = NYA_GAMEPAD_AXIS_LEFT_X;
            if (!_nya_gamepad_axis_from_sdl(event->gaxis.axis, &axis)) return true;

            slot->axes[axis] = event->gaxis.value;
            return true;
        }

        default: return false;
    }
#endif
}

u32 nya_gamepad_count(void) {
    u32 count = 0;
    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        if (_nya_gamepad_system.slots[i].open) count++;
    }
    return count;
}

NYA_GamepadId nya_gamepad_at(u32 index) {
    u32 seen = 0;
    for (u32 i = 0; i < NYA_GAMEPAD_MAX; i++) {
        if (!_nya_gamepad_system.slots[i].open) continue;
        if (seen++ == index) return _nya_gamepad_system.slots[i].id;
    }
    return NYA_GAMEPAD_NONE;
}

b8 nya_gamepad_connected(NYA_GamepadId pad) {
    return _nya_gamepad_slot(pad) != nullptr;
}

NYA_ConstCString nya_gamepad_name(NYA_GamepadId pad) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr) return "";

#ifdef NYA_NO_SDL
    return "";
#else
    NYA_ConstCString name = SDL_GetGamepadName(slot->handle);
    return name != nullptr ? name : "";
#endif
}

NYA_GamepadKind nya_gamepad_kind(NYA_GamepadId pad) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    return slot != nullptr ? slot->kind : NYA_GAMEPAD_KIND_UNKNOWN;
}

b8 nya_gamepad_button_pressed(NYA_GamepadId pad, NYA_GamepadButton button) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || button >= NYA_GAMEPAD_BUTTON_COUNT) return false;
    return slot->pressed[button];
}

b8 nya_gamepad_button_just_pressed(NYA_GamepadId pad, NYA_GamepadButton button) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || button >= NYA_GAMEPAD_BUTTON_COUNT) return false;
    return slot->just_pressed[button];
}

b8 nya_gamepad_button_just_released(NYA_GamepadId pad, NYA_GamepadButton button) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || button >= NYA_GAMEPAD_BUTTON_COUNT) return false;
    return slot->just_released[button];
}

f32 nya_gamepad_axis(NYA_GamepadId pad, NYA_GamepadAxis axis) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || axis >= NYA_GAMEPAD_AXIS_COUNT) return 0.0F;

    b8 is_trigger = axis == NYA_GAMEPAD_AXIS_LEFT_TRIGGER || axis == NYA_GAMEPAD_AXIS_RIGHT_TRIGGER;

    f32 value = _nya_gamepad_normalize(slot->axes[axis], is_trigger ? NYA_GAMEPAD_TRIGGER_DEADZONE : NYA_GAMEPAD_STICK_DEADZONE);

    // A trigger rests at zero and only pulls one way, so its negative half is not a direction.
    return is_trigger ? nya_clamp(value, 0.0F, 1.0F) : value;
}

f32x2 nya_gamepad_stick(NYA_GamepadId pad, b8 right_stick) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr) return f32x2_zero;

    NYA_GamepadAxis x_axis = right_stick ? NYA_GAMEPAD_AXIS_RIGHT_X : NYA_GAMEPAD_AXIS_LEFT_X;
    NYA_GamepadAxis y_axis = right_stick ? NYA_GAMEPAD_AXIS_RIGHT_Y : NYA_GAMEPAD_AXIS_LEFT_Y;

    f32x2 raw = { (f32)slot->axes[x_axis] / 32767.0F, (f32)slot->axes[y_axis] / 32767.0F };

    /*
     * Radial, not per axis.
     *
     * Deadzoning each axis on its own leaves a cross-shaped live region: a diagonal registers once
     * either component clears the threshold, so a diagonal is easier to trigger than a cardinal and the
     * stick feels square.
     */
    f32 magnitude = nya_vector_length(raw);
    if (magnitude < NYA_GAMEPAD_STICK_DEADZONE) return f32x2_zero;

    f32 scaled = nya_min((magnitude - NYA_GAMEPAD_STICK_DEADZONE) / (1.0F - NYA_GAMEPAD_STICK_DEADZONE), 1.0F);

    return (raw / magnitude) * scaled;
}

b8 nya_gamepad_has_rumble(NYA_GamepadId pad) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    return slot != nullptr && slot->has_rumble;
}

void nya_gamepad_rumble(NYA_GamepadId pad, f32 low_frequency, f32 high_frequency, u32 duration_ms) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || !slot->has_rumble) return;

#ifndef NYA_NO_SDL
    u16 low  = (u16)(nya_clamp(low_frequency, 0.0F, 1.0F) * 65535.0F);
    u16 high = (u16)(nya_clamp(high_frequency, 0.0F, 1.0F) * 65535.0F);

    (void)SDL_RumbleGamepad(slot->handle, low, high, duration_ms);
#else
    nya_unused(low_frequency, high_frequency, duration_ms);
#endif
}

void nya_gamepad_rumble_stop(NYA_GamepadId pad) {
    nya_gamepad_rumble(pad, 0.0F, 0.0F, 0);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * AXES AS BUTTONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether a raw axis reading is past `threshold`, with the threshold's sign meaning its direction. */
NYA_INTERNAL b8 _nya_gamepad_axis_past(s16 raw, f32 threshold) {
    if (threshold == 0.0F) threshold = NYA_GAMEPAD_TRIGGER_THRESHOLD;

    f32 value = nya_clamp((f32)raw / 32767.0F, -1.0F, 1.0F);

    // The sign is the direction: -0.5 means "pushed left past halfway", not "further than -0.5".
    return threshold < 0.0F ? value <= threshold : value >= threshold;
}

b8 nya_gamepad_axis_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || axis >= NYA_GAMEPAD_AXIS_COUNT) return false;

    return _nya_gamepad_axis_past(slot->axes[axis], threshold);
}

b8 nya_gamepad_axis_just_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || axis >= NYA_GAMEPAD_AXIS_COUNT) return false;

    return _nya_gamepad_axis_past(slot->axes[axis], threshold) && !_nya_gamepad_axis_past(slot->axes_previous[axis], threshold);
}

b8 nya_gamepad_axis_just_released(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) {
    _NYA_GamepadSlot* slot = _nya_gamepad_slot(pad);
    if (slot == nullptr || axis >= NYA_GAMEPAD_AXIS_COUNT) return false;

    return !_nya_gamepad_axis_past(slot->axes[axis], threshold) && _nya_gamepad_axis_past(slot->axes_previous[axis], threshold);
}
