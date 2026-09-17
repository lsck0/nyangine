/**
 * @file ui.h
 *
 * Immediate-mode menus and HUD panels. A layer describes its UI with plain calls each time it runs; the module
 * keeps only what has to outlive a call: the focused widget, the one the pointer holds, and each panel's size.
 * Everything sits in fixed tables, so a frame allocates nothing.
 *
 * Functions
 *
 *   nya_ui_begin, nya_ui_end              one pass over a window's UI, reading input or drawing
 *   nya_ui_panel_begin, nya_ui_panel_end  an anchored rounded box that stacks its widgets, or nested, a column
 *   nya_ui_row_begin, nya_ui_row_end      the next widgets share one row in equal cells
 *   nya_ui_label                          text
 *   nya_ui_button                         true on the pass it is activated
 *   nya_ui_selectable                     a button that shows whether it is the chosen one
 *   nya_ui_toggle                         flips a b8
 *   nya_ui_slider                         moves an f32 between two bounds in steps
 *   nya_ui_space                          room in the layout for custom drawing
 *   nya_ui_scrim                          dims the whole window
 *   nya_ui_cancelled                      whether cancel was pressed this pass
 *   nya_ui_last_rect                      where the last widget went
 *   nya_ui_focus_reset                    focus goes back to the first widget
 *   nya_ui_modal_event                    stops key and mouse events at a modal layer
 *   nya_ui_style_set, nya_ui_style_get    the look, per window
 *
 * ```c
 * NYA_INTERNAL void pause_menu(NYA_Window* window, NYA_UIPass pass) {
 *     NYA_UI* ui = nya_ui_begin(window, pass);
 *     nya_ui_scrim(ui);
 *
 *     if (nya_ui_panel_begin(ui, "pause", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = 360, .align = NYA_UI_ALIGN_CENTER, .title = "paused" })) {
 *         if (nya_ui_button(ui, "resume")) resume();
 *         (void)nya_ui_slider(ui, "volume", &volume, 0.0F, 1.0F, 0.05F);
 *         if (nya_ui_cancelled(ui)) resume();
 *         nya_ui_panel_end(ui);
 *     }
 *
 *     nya_ui_end(ui);
 * }
 *
 * void on_update(NYA_Window* window, f32 delta_time_s) { pause_menu(window, NYA_UI_PASS_INPUT); }
 * void on_render(NYA_Window* window) { pause_menu(window, NYA_UI_PASS_DRAW); }
 * void on_event(NYA_Window* window, NYA_Event* event) { (void)nya_ui_modal_event(event); }
 * ```
 *
 * Why it looks like this
 *
 * - Input is read per tick and drawing happens per frame, by running the same function twice. Key, mouse button and
 *   gamepad edges roll at the end of each update tick, so an input pass in on_update sees every press exactly once:
 *   a frame with no tick reads nothing and draws the last state, a frame with two ticks reads the press in the first.
 *   What a widget returns lands inside the simulation, before the barrier, like any other gameplay change. A draw
 *   pass returns false from every widget, so reading input straight from the input system inside that function
 *   would act twice; read it through the UI or in on_update.
 * - Ids hash the label into the enclosing panel's id, so there is nothing to declare. When the focused id
 *   disappears, as every label does when the locale changes, focus stays on the same position in the list.
 * - A panel is positioned from its size in the previous pass, which is what lets it be centred or anchored to a
 *   corner without measuring twice. A panel seen for the first time lays out without drawing for that one pass.
 * - Rejected: recording draw commands in the tick and replaying them in on_render, which needs a text pool, cannot
 *   host custom drawing inside a panel, and draws a tick-old state. Acting on input from on_render, which runs
 *   gameplay from the renderer and loses presses while a window is minimised. A retained widget tree, which is state
 *   to keep in sync with the game for menus that are a dozen rows. Text input, which needs an IME caret and
 *   selection and has no caller yet.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_event.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/renderer/render_font.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Focusable widgets in one pass. Past this a widget is refused: it draws nothing and returns false. */
#ifndef NYA_UI_WIDGETS_MAX
#define NYA_UI_WIDGETS_MAX 64
#endif

/** Panel sizes remembered across every window. A pass that opens more is refused the extra panels. */
#ifndef NYA_UI_PANELS_MAX
#define NYA_UI_PANELS_MAX 32
#endif

/** Panels and rows open inside each other at once. */
#ifndef NYA_UI_DEPTH_MAX
#define NYA_UI_DEPTH_MAX 8
#endif

/** Longest registered font name a style can hold, terminator included. */
#define NYA_UI_FONT_NAME_MAX 32

/** How long a direction is held before it repeats, and how often it repeats after that, in seconds. */
#define NYA_UI_REPEAT_DELAY_S    0.35F
#define NYA_UI_REPEAT_INTERVAL_S 0.08F

/** How long a newly focused widget takes to settle from its pop. */
#define NYA_UI_POP_S 0.15F

/* What a zeroed style field becomes. A soft cream card with a bold ink outline and a warm accent. */

#define NYA_UI_PADDING     12.0F
#define NYA_UI_SPACING     8.0F
#define NYA_UI_RADIUS      12.0F
#define NYA_UI_OUTLINE     3.0F
#define NYA_UI_DEPTH       4.0F
#define NYA_UI_POP         4.0F
#define NYA_UI_SCRIM       ((NYA_Color){ 0.05F, 0.04F, 0.10F, 0.55F })
#define NYA_UI_PANEL       ((NYA_Color){ 0.99F, 0.96F, 0.88F, 0.96F })
#define NYA_UI_INK         ((NYA_Color){ 0.13F, 0.10F, 0.18F, 1.0F })
#define NYA_UI_TEXT        ((NYA_Color){ 0.13F, 0.10F, 0.18F, 1.0F })
#define NYA_UI_TEXT_DIM    ((NYA_Color){ 0.45F, 0.40F, 0.46F, 1.0F })
#define NYA_UI_BUTTON      ((NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F })
#define NYA_UI_ACCENT      ((NYA_Color){ 1.0F, 0.74F, 0.25F, 1.0F })
#define NYA_UI_ACCENT_TEXT ((NYA_Color){ 0.13F, 0.10F, 0.18F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_UI      NYA_UI;
typedef struct NYA_UIStyle NYA_UIStyle;
typedef struct NYA_UIPanel NYA_UIPanel;

/** What a pass over the UI does. See the file header for why there are two. */
typedef enum NYA_UIPass {
    /** From on_update: reads this tick's input, moves focus and values, and draws nothing. */
    NYA_UI_PASS_INPUT = 0,

    /** From on_render: draws, and every widget returns false. */
    NYA_UI_PASS_DRAW,

    NYA_UI_PASS_COUNT,
} NYA_UIPass;

/** Where a top level panel sits in the window. Nested panels are placed by their parent instead. */
typedef enum NYA_UIAnchor {
    NYA_UI_ANCHOR_TOP_LEFT = 0,
    NYA_UI_ANCHOR_TOP,
    NYA_UI_ANCHOR_TOP_RIGHT,
    NYA_UI_ANCHOR_LEFT,
    NYA_UI_ANCHOR_CENTER,
    NYA_UI_ANCHOR_RIGHT,
    NYA_UI_ANCHOR_BOTTOM_LEFT,
    NYA_UI_ANCHOR_BOTTOM,
    NYA_UI_ANCHOR_BOTTOM_RIGHT,

    NYA_UI_ANCHOR_COUNT,
} NYA_UIAnchor;

/** How a widget narrower than its column or cell sits in it. Buttons, sliders and toggles fill it. */
typedef enum NYA_UIAlign {
    NYA_UI_ALIGN_START = 0,
    NYA_UI_ALIGN_CENTER,
    NYA_UI_ALIGN_END,

    NYA_UI_ALIGN_COUNT,
} NYA_UIAlign;

/**
 * The look of every widget in a window. A zeroed field, or a colour with all four channels zero, takes its
 * NYA_UI_* default, so a zeroed style is the default look.
 * */
// @reflect
struct NYA_UIStyle {
    /** Registered font names, see nya_font_register. Empty uses nya_font_default for both. */
    char font[NYA_UI_FONT_NAME_MAX];
    char title_font[NYA_UI_FONT_NAME_MAX];

    /** Inside a panel's edge, and around a button's label. */
    f32 padding;

    /** Between widgets. */
    f32 spacing;

    f32 radius;

    /** The ink line around panels and widgets. */
    f32 outline;

    /** How far the ink shadow drops under a widget, and so how far a pressed one sinks. */
    f32 depth;

    /** How much a newly focused widget grows before settling. */
    f32 pop;

    /** A button, slider, toggle or selectable's height. Zero is the font's line height plus padding. */
    f32 item_height;

    NYA_Color scrim;
    NYA_Color panel;
    NYA_Color ink;
    NYA_Color text;
    NYA_Color text_dim;
    NYA_Color button;

    /** The focused widget's fill, and the filled part of a slider or toggle. */
    NYA_Color accent;
    NYA_Color accent_text;
};

struct NYA_UIPanel {
    NYA_UIAnchor anchor;

    /** Pixels in from the anchored edges. Ignored on the axis an anchor centres. */
    f32x2 offset;

    /** Zero fits the content, from the previous pass. */
    f32 width;
    f32 height;

    NYA_UIAlign align;

    /** Drawn centred at the top in the style's title font. Optional. */
    NYA_ConstCString title;

    /** Overrides the style's font for everything inside. NYA_FONT_NONE keeps it. */
    NYA_Font font;

    /** No background, outline or padding: a plain column. */
    b8 frameless;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * PASSES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts a pass over `window`'s UI. One pass is open at a time; end it before beginning another.
 * */
NYA_API NYA_UI* nya_ui_begin(NYA_Window* window, NYA_UIPass pass) __attr_no_discard;

/** Closes the pass. An input pass moves focus here, so a confirm and a direction in one tick act on the old focus. */
NYA_API void nya_ui_end(NYA_UI* ui);

/*
 * ─────────────────────────────────────────────────────────
 * LAYOUT
 * ─────────────────────────────────────────────────────────
 */

/**
 * Opens a panel. `id` names it for its remembered size and scopes the ids of what is inside.
 *
 * False when the panel table is full with panels this pass already opened; skip the contents and the end.
 * */
NYA_API b8   nya_ui_panel_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel) __attr_no_discard;
NYA_API void nya_ui_panel_end(NYA_UI* ui);

/**
 * Opens a row of `columns` equal cells across the enclosing width. In a panel that fits its content, cells take
 * their widgets' natural widths instead.
 * */
NYA_API void nya_ui_row_begin(NYA_UI* ui, u32 columns);
NYA_API void nya_ui_row_end(NYA_UI* ui);

/** Takes room in the layout and returns it, for drawing into during the draw pass. A zero width fills the column. */
NYA_API NYA_Rectf nya_ui_space(NYA_UI* ui, f32 width, f32 height);

/** Dims the whole window in the style's scrim colour, under whatever is drawn after it. */
NYA_API void nya_ui_scrim(NYA_UI* ui);

/*
 * ─────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────
 */

/** Text in the enclosing font, in the style's text colour or in `color`. */
NYA_API void nya_ui_label(NYA_UI* ui, NYA_ConstCString text) __attr_overloaded;
NYA_API void nya_ui_label(NYA_UI* ui, NYA_ConstCString text, NYA_Color color) __attr_overloaded;

/** True on the pass it is activated: confirm while focused, or a left click released over it. */
NYA_API b8 nya_ui_button(NYA_UI* ui, NYA_ConstCString label) __attr_no_discard;

/** A button marked when `selected`, for picking one of several. True when activated. */
NYA_API b8 nya_ui_selectable(NYA_UI* ui, NYA_ConstCString label, b8 selected) __attr_no_discard;

/** Activating flips `*value`; left and right set it off and on. True when it changed. */
NYA_API b8 nya_ui_toggle(NYA_UI* ui, NYA_ConstCString label, b8* value);

/**
 * Left and right move `*value` by `step`, dragging sets it from the pointer snapped to `step`, and it stays in
 * [`min`, `max`]. True when it changed.
 * */
NYA_API b8 nya_ui_slider(NYA_UI* ui, NYA_ConstCString label, f32* value, f32 min, f32 max, f32 step);

/*
 * ─────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────
 */

/** Whether cancel was pressed this pass. Always false in a draw pass. */
NYA_API b8 nya_ui_cancelled(const NYA_UI* ui) __attr_no_discard;

/** The window rect the last widget, row or panel took. */
NYA_API NYA_Rectf nya_ui_last_rect(const NYA_UI* ui) __attr_no_discard;

/** Moves focus to the first widget of the next pass and drops any press. For a menu that opens fresh. */
NYA_API void nya_ui_focus_reset(NYA_Window* window);

/**
 * Marks key, text, mouse button and wheel events handled, so layers under a modal UI never see them. The UI reads
 * the input state, which took the event before any layer did. True when it took the event.
 * */
NYA_API b8 nya_ui_modal_event(NYA_Event* event);

/*
 * ─────────────────────────────────────────────────────────
 * STYLE
 * ─────────────────────────────────────────────────────────
 */

/** Replaces `window`'s style. Cheap, so a game can hand it a hot reloaded config every frame. */
NYA_API void nya_ui_style_set(NYA_Window* window, NYA_UIStyle style);

/** The style in use, with every zeroed field filled in. */
NYA_API NYA_UIStyle nya_ui_style_get(const NYA_Window* window) __attr_no_discard;
