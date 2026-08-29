/**
 * Gamepads with no gamepad attached: the queries every game makes before anyone plugs one in.
 *
 * A CI machine has no controller, so what is testable here is the half that must be right anyway —
 * that every query on an absent pad answers rather than crashing, that the deadzone maths is correct,
 * and that a gamepad binding is a first-class binding rather than a special case. Those are also the
 * paths a real pad never exercises, so they are exactly the ones a test has to cover.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_gamepad_init();
    defer nya_system_gamepad_deinit();

    // ── With nothing connected, every query answers rather than crashing.
    {
        nya_check(nya_gamepad_count() == 0, "no pads on a test machine, got %u", nya_gamepad_count());
        nya_check(nya_gamepad_at(0) == NYA_GAMEPAD_NONE, "there is no first pad");
        nya_check(nya_gamepad_at(99) == NYA_GAMEPAD_NONE, "nor a hundredth");
        nya_check(!nya_gamepad_connected(NYA_GAMEPAD_NONE), "the none-pad is not connected");
        nya_check(!nya_gamepad_connected(12345), "nor an arbitrary id");

        nya_check(!nya_gamepad_button_pressed(NYA_GAMEPAD_NONE, NYA_GAMEPAD_BUTTON_SOUTH), "no buttons are held");
        nya_check(!nya_gamepad_button_just_pressed(NYA_GAMEPAD_NONE, NYA_GAMEPAD_BUTTON_START), "no edges either");
        nya_check(!nya_gamepad_button_just_released(NYA_GAMEPAD_NONE, NYA_GAMEPAD_BUTTON_START), "in either direction");

        nya_check(nya_gamepad_axis(NYA_GAMEPAD_NONE, NYA_GAMEPAD_AXIS_LEFT_X) == 0.0F, "axes read zero");

        f32x2 stick = nya_gamepad_stick(NYA_GAMEPAD_NONE, false);
        nya_check(stick.x == 0.0F && stick.y == 0.0F, "and so does a stick");

        nya_check(!nya_gamepad_has_rumble(NYA_GAMEPAD_NONE), "an absent pad cannot rumble");
        nya_check(nya_gamepad_kind(NYA_GAMEPAD_NONE) == NYA_GAMEPAD_KIND_UNKNOWN, "and has no known kind");
        nya_check(nya_gamepad_name(NYA_GAMEPAD_NONE)[0] == '\0', "and no name");

        // Must be silent no-ops rather than errors: a game should not have to branch on rumble support.
        nya_gamepad_rumble(NYA_GAMEPAD_NONE, 1.0F, 1.0F, 100);
        nya_gamepad_rumble_stop(NYA_GAMEPAD_NONE);
    }

    // ── An out-of-range button or axis is refused rather than read past the array.
    {
        nya_check(!nya_gamepad_button_pressed(NYA_GAMEPAD_NONE, NYA_GAMEPAD_BUTTON_COUNT), "a past-the-end button is not pressed");
        nya_check(nya_gamepad_axis(NYA_GAMEPAD_NONE, NYA_GAMEPAD_AXIS_COUNT) == 0.0F, "a past-the-end axis is zero");
    }

    // ── Frame begin and a stray event are safe with nothing connected.
    {
        nya_system_gamepad_frame_begin();
        nya_check(!nya_system_gamepad_handle_sdl_event(nullptr), "a null event is not a gamepad event");
    }

    // ── The deadzone rescales rather than clamping, so a control eases in instead of snapping.
    {
        // _nya_gamepad_normalize is internal, but it is the thing worth pinning: just past the dead
        // zone must be near zero, not a jump to the dead zone's own value.
        f32 just_past = _nya_gamepad_normalize((s16)(0.19F * 32767.0F), NYA_GAMEPAD_STICK_DEADZONE);
        f32 well_past = _nya_gamepad_normalize((s16)(0.60F * 32767.0F), NYA_GAMEPAD_STICK_DEADZONE);
        f32 full      = _nya_gamepad_normalize(32767, NYA_GAMEPAD_STICK_DEADZONE);

        nya_check(just_past > 0.0F && just_past < 0.05F, "just past the dead zone should be near zero, got %f", (f64)just_past);
        nya_check(well_past > just_past, "and it should rise from there");
        nya_check(fabsf(full - 1.0F) < 0.001F, "full deflection should still reach 1, got %f", (f64)full);

        nya_check(_nya_gamepad_normalize(0, NYA_GAMEPAD_STICK_DEADZONE) == 0.0F, "centre is zero");
        nya_check(_nya_gamepad_normalize((s16)(0.10F * 32767.0F), NYA_GAMEPAD_STICK_DEADZONE) == 0.0F,
                  "inside the dead zone is zero");

        f32 negative = _nya_gamepad_normalize((s16)(-0.60F * 32767.0F), NYA_GAMEPAD_STICK_DEADZONE);
        nya_check(negative < 0.0F, "the sign should survive, got %f", (f64)negative);
        nya_check(fabsf(negative + well_past) < 0.001F, "and be symmetric");
    }

    // ── An axis threshold's sign is its direction, not a magnitude.
    {
        s16 pushed_left  = (s16)(-0.80F * 32767.0F);
        s16 pushed_right = (s16)(0.80F * 32767.0F);

        nya_check(_nya_gamepad_axis_past(pushed_left, -0.5F), "left past a negative threshold is pressed");
        nya_check(!_nya_gamepad_axis_past(pushed_left, 0.5F), "but not past a positive one");
        nya_check(_nya_gamepad_axis_past(pushed_right, 0.5F), "and right past a positive threshold is");
        nya_check(!_nya_gamepad_axis_past(pushed_right, -0.5F), "but not past a negative one");

        nya_check(!_nya_gamepad_axis_past(0, 0.5F), "centre is not past anything");
        nya_check(_nya_gamepad_axis_past((s16)(0.9F * 32767.0F), 0.0F), "a zero threshold uses the trigger default");
    }

    // ── A gamepad binding is a first-class binding: bound, queryable, and cleared like any other.
    {
        // The input system registers event hooks, so the event system has to be up first.
        nya_system_callback_init();
        NYA_EXPECT(nya_system_events_init());
        nya_system_input_init();

        defer nya_system_input_deinit();
        defer nya_system_events_deinit();
        defer nya_system_callback_deinit();

        const NYA_InputAction jump = NYA_INPUT_ACTION_USER;

        nya_check(!nya_input_action_bound(jump), "nothing is bound yet");

        nya_input_action_bind_button(jump, NYA_GAMEPAD_BUTTON_SOUTH);
        nya_check(nya_input_action_bound(jump), "a gamepad-only binding must count as bound");

        NYA_InputBinding binding = nya_input_action_get(jump, 0);
        nya_check(binding.kind == NYA_INPUT_BINDING_GAMEPAD_BUTTON, "the slot should carry the gamepad tag");
        nya_check(binding.button == NYA_GAMEPAD_BUTTON_SOUTH, "and the button");
        nya_check(binding.key == NYA_KEY_UNKNOWN, "and no key");

        // Nothing is connected, so it cannot be pressed — but it must answer, not crash.
        nya_check(!nya_input_action_pressed(jump), "an unpressable binding is simply not pressed");
        nya_check(!nya_input_binding_gamepad_pressed(binding), "and the direct query agrees");

        // A keyboard binding alongside it keeps its own identity.
        nya_input_action_bind(jump, NYA_KEY_SPACE);
        NYA_InputBinding second = nya_input_action_get(jump, 1);
        nya_check(second.kind == NYA_INPUT_BINDING_KEY, "the second slot should be a key binding");
        nya_check(second.key == NYA_KEY_SPACE, "with the key it was given");

        nya_input_action_unbind(jump);
        nya_check(!nya_input_action_bound(jump), "unbinding should clear both");

        // An axis binding round trips its threshold, sign included.
        nya_input_action_bind_axis(jump, NYA_GAMEPAD_AXIS_LEFT_X, -0.5F);
        NYA_InputBinding axis = nya_input_action_get(jump, 0);
        nya_check(axis.kind == NYA_INPUT_BINDING_GAMEPAD_AXIS, "an axis binding should carry its tag");
        nya_check(axis.axis == NYA_GAMEPAD_AXIS_LEFT_X, "and the axis");
        nya_check(axis.axis_threshold == -0.5F, "and the signed threshold, got %f", (f64)axis.axis_threshold);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
