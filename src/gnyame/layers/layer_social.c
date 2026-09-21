/**
 * @file layer_social.c
 *
 * The "somebody wants to join" prompt. Pushed on top of whatever is on screen when a join request
 * arrives, popped when it is answered, so it works from the main menu and from inside a session alike.
 *
 * The same function reads input on update and draws on render, see ui.h.
 * */
#include "gnyame/gnyame.h"

NYA_INTERNAL void _gny_social_prompt(NYA_Window* window, NYA_UIPass pass);

void gny_layer_social_on_create(NYA_Window* window) {
    // on "accept", so enter answers the common case. A request that has already been answered by the
    // time this runs leaves the layer to pop itself on the next update.
    nya_ui_focus_reset(window);
}

void gny_layer_social_on_destroy(NYA_Window* window) {
    nya_ui_focus_reset(window);
}

void gny_layer_social_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    // modal: a click on the prompt must not also drop a crate in the world behind it, and the layer
    // sits over a running game rather than over a stopped one.
    (void)nya_ui_modal_event(event);
}

void gny_layer_social_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);

    // the request went away while the prompt was up: the provider expired it, or the connection
    // dropped. Nothing to answer, so the prompt goes rather than sitting there lying.
    if (!gny_social_request_pending()) {
        gny_screen_request(GNY_SCREEN_SOCIAL_DISMISS);
        return;
    }

    _gny_social_prompt(window, NYA_UI_PASS_INPUT);
}

void gny_layer_social_on_render(NYA_Window* window) {
    if (!gny_social_request_pending()) return;

    _gny_social_prompt(window, NYA_UI_PASS_DRAW);
}

void _gny_social_prompt(NYA_Window* window, NYA_UIPass pass) {
    NYA_UI* ui = gny_ui_begin(window, pass);
    nya_ui_scrim(ui);

    NYA_UIPanel panel = {
        .anchor = NYA_UI_ANCHOR_CENTER,
        .width  = nya_ui_fixed(GNY_MENU_WIDTH),
        .align  = NYA_UI_ALIGN_CENTER,
        .title  = nya_string_social_join_request(),
    };

    if (nya_ui_panel_begin(ui, "social_prompt", panel)) {
        // the name is another player's display name, so it is shown as a label and never as a format
        // string; nya_ui_label takes it as text.
        nya_ui_label(ui, gny_social_request_name());

        nya_ui_label(ui, nya_string_social_wants_to_join(), nya_ui_style_get(window).text_dim);

        if (nya_ui_button(ui, nya_string_social_accept())) gny_social_request_answer(true);
        if (nya_ui_button(ui, nya_string_social_decline())) gny_social_request_answer(false);

        // the dismiss key declines, which is the safe answer to a question the player did not ask for.
        if (nya_ui_cancelled(ui)) gny_social_request_answer(false);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}
