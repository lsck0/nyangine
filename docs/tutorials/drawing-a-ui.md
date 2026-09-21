# Drawing a UI

The UI is immediate mode: you describe the interface every frame and it has no retained tree of its
own. There is no widget to create, keep a pointer to, and destroy.

## The two passes

One function describes the interface, and it runs **twice** per frame — once reading input during
`on_update`, once drawing during `on_render`. Write it once and pass the mode through:

```c
static void my_menu(NYA_Window* window, NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(window, pass);

    if (nya_ui_panel_begin(ui, "menu", (NYA_UIPanel){
            .anchor = NYA_UI_ANCHOR_CENTER,
            .width  = nya_ui_fixed(320.0F),
            .align  = NYA_UI_ALIGN_CENTER,
            .title  = "menu",
        })) {

        nya_ui_label(ui, "how many crates?");

        if (nya_ui_button(ui, "spawn")) spawn_a_crate();
        if (nya_ui_button(ui, "quit"))  nya_app_get()->should_quit = true;

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

void my_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);
    my_menu(window, NYA_UI_PASS_INPUT);
}

void my_layer_on_render(NYA_Window* window) {
    my_menu(window, NYA_UI_PASS_DRAW);
}
```

Why two passes rather than one: input has to be resolved before anything draws, or a button drawn
this frame reacts to a click next frame. Splitting them means a press is handled exactly once however
many fixed ticks a frame happens to run.

`nya_ui_button` returns true on the pass it is activated — confirm while focused, or a left click
released over it. So the call site reads as the action, not as a state query.

## Layout

Containers are rows or columns, and a child's size is one of four things:

| | |
| :--- | :--- |
| `nya_ui_fixed(n)` | Exactly `n` pixels |
| `nya_ui_fit()` | As large as its content |
| `nya_ui_grow(n)` | Shares the leftover space, weighted |
| Auto | The style's default for that widget |

Panels nest, scroll on both axes when their content overflows, and clip. A panel with
`.frameless = true` draws no background, which is how you group things for layout without it looking
like a box.

## Scale

The UI does **not** follow the window size. `NYA_UIStyle.scale` is the only thing that moves it, with
`follow_display_scale` as an opt-in for HiDPI.

That is deliberate and was once the other way. Deriving scale from window height meant every scale
step minted a new font size, each needing its own glyph atlas, and the atlas cache filled and then
refused every new size for the rest of the run — text drew blank after a resize. Sizes a player
chooses are a handful; sizes a window drag produces are unbounded.

## Styling

The style is a plain struct where zero is the default look, fed from `engine.ui` in the config and
hot reloaded with it. Push and pop it to change part of a tree:

```c
NYA_UIStyle style = nya_ui_style_get(window);
style.accent = (NYA_Color){ 0.9F, 0.3F, 0.4F, 1.0F };

nya_ui_style_push(ui, style);
// ... widgets here are accented ...
nya_ui_style_pop(ui);
```

Nine-slice skins cut from a sheet replace the flat look per element and per state, which is how a
game UI stops looking like a debug overlay without any code changing.

## What is there

Label, button, selectable, radio, toggle, slider, single line text input with full editing, colour
picker, tabs, dropdown, table, line and bar charts, icon, space and scrim. Panels drag by their
title, opacity groups fade a whole subtree, and widgets pop on focus and bounce on activation.

Not there yet: a floating dropdown (an immediate pass has no z-order, so an open list takes room in
the layout), a node editor, SVG, and a multi-line code editor.

See the [cheatsheet](../CHEATSHEET.md) for every signature, and `src/nyangine/ui/ui.h` for the
reasoning behind each.
