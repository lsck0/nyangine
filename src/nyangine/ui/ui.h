/**
 * @file ui.h
 *
 * Immediate-mode menus and HUD panels. A layer describes its UI with plain calls each time it runs; the module
 * keeps only what has to outlive a call: the focused widget, the one the pointer holds, the field being typed into,
 * and each container's measurements and scroll. Everything sits in fixed tables, so a frame allocates nothing.
 *
 * Functions
 *
 *   nya_ui_begin, nya_ui_end                    one pass over a window's UI, reading input or drawing
 *   nya_ui_panel_begin, nya_ui_panel_end        a container stacking its children down or across, framed or plain
 *   nya_ui_size                                 the next child's size along its container's direction
 *   nya_ui_label                                text, wrapped or shrunk to fit when the container says so
 *   nya_ui_button                               true on the pass it is activated
 *   nya_ui_selectable                           a button that shows whether it is the chosen one
 *   nya_ui_toggle                               flips a b8
 *   nya_ui_slider                               moves an f32 between two bounds in steps
 *   nya_ui_text_input                           one line of typed text in a caller's buffer
 *   nya_ui_color_picker                         a colour by saturation and value, hue, alpha and hex
 *   nya_ui_space                                room in the layout for custom drawing, or a spacer
 *   nya_ui_scrim                                dims the whole window
 *   nya_ui_disabled_begin, nya_ui_disabled_end  widgets between them are dimmed, skipped by focus, and never act
 *   nya_ui_style_push, nya_ui_style_pop         another look for what follows, until popped
 *   nya_ui_cancelled                            whether cancel was pressed this pass
 *   nya_ui_typing                               whether a field has the keyboard, so a layer can skip its own keys
 *   nya_ui_focus_reset                          focus goes back to the first widget
 *   nya_ui_modal_event                          stops key and mouse events at a modal layer
 *   nya_ui_style_set, nya_ui_style_get          the look, per window
 *   nya_ui_scale                                what sizes were multiplied by in the window's last pass
 *
 * ```c
 * NYA_INTERNAL void pause_menu(NYA_Window* window, NYA_UIPass pass) {
 *     NYA_UI* ui = nya_ui_begin(window, pass);
 *     nya_ui_scrim(ui);
 *
 *     if (nya_ui_panel_begin(ui, "pause", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(360), .align = NYA_UI_ALIGN_CENTER, .title = "paused" })) {
 *         if (nya_ui_button(ui, "resume")) resume();
 *         (void)nya_ui_slider(ui, "volume", &volume, 0.0F, 1.0F, 0.05F);
 *
 *         // a row: the label as wide as its text, the choices sharing the rest.
 *         if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
 *             nya_ui_label(ui, "language");
 *             nya_ui_size(ui, nya_ui_grow(1));
 *             if (nya_ui_selectable(ui, "English", english)) pick(0);
 *             nya_ui_size(ui, nya_ui_grow(1));
 *             if (nya_ui_selectable(ui, "Deutsch", !english)) pick(1);
 *             nya_ui_panel_end(ui);
 *         }
 *
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
 * - Ids hash the label into the enclosing named panel's id, so there is nothing to declare. An unnamed container
 *   scopes nothing, so wrapping widgets in a row keeps their ids. When the focused id disappears, as every label does
 *   when the locale changes, focus stays on the same position in the list.
 * - Layout is one pass, sized from the previous one. A container remembers what its content measured, what its
 *   fixed and fitting children took and how much weight its growing children asked for; this pass hands the rest out
 *   by weight, rounded so the shares add up to the pixel. That is what lets a panel be centred, fit its content or
 *   split a row without measuring twice. A container seen for the first time lays out without drawing for one pass.
 * - One container does both directions. Along its direction a child is fixed, fits, or grows into a weighted share;
 *   across it, panels and widgets that fill (buttons, sliders, toggles, fields) take the whole extent and the rest
 *   fit and follow the container's alignment. A spacer is a growing nya_ui_space.
 * - Sizes are written for a window `reference_height` tall and multiplied by a per window scale, derived from the
 *   window's height and never below the display's own scale, in quarter steps so a resize bakes few glyph atlases.
 *   Fonts are rasterised at the scaled size, so text stays crisp. Top level panels sit inside the window's safe area,
 *   `margin` in from it, and scroll when they would not fit.
 * - Focus moves through rows as lines: up and down go to the next line, left and right between the focusable cells
 *   of one row. Sliders, toggles and a field being typed into keep left and right for themselves.
 * - A field types only after confirm or a click, because menu keys are letters too (W, A, S, D and space), and
 *   stops on return, cancel or a click elsewhere. A confirm that also typed text is the space bar, not confirm.
 * - The default look is flat and quiet: no outline, no shadow, no pop, neutral fills and one accent marking focus
 *   and values, a debug UI. The style dresses it up, per window or pushed for one part: sizes and colours per state,
 *   and nine-slice skins per element and state, cut from a sheet and scaled with the UI.
 * - Animation is declared, not driven: focus and press colours ease over `transition_s`, and a top level panel slides
 *   in over `appear_s` when it shows again, on the wall clock so a paused simulation still animates. Each widget's
 *   progress sits in a small direct mapped table by id; a collision only snaps a transition. Zero durations never
 *   touch the table.
 * - Rejected: recording draw commands in the tick and replaying them in on_render, which needs a text pool, cannot
 *   host custom drawing inside a panel, and draws a tick-old state. Acting on input from on_render, which runs
 *   gameplay from the renderer and loses presses while a window is minimised. A retained widget tree, which is state
 *   to keep in sync with the game for menus that are a dozen rows. Selection and clipboard in a field, which a name
 *   or a seed does not need.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_event.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/math/math_tween.h"
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

/** Container measurements remembered across every window. A pass that opens more is refused the extra containers. */
#ifndef NYA_UI_PANELS_MAX
#define NYA_UI_PANELS_MAX 64
#endif

/** Containers open inside each other at once. */
#ifndef NYA_UI_DEPTH_MAX
#define NYA_UI_DEPTH_MAX 8
#endif

/** Styles pushed on top of the window's at once. */
#define NYA_UI_STYLE_DEPTH_MAX 4

/** Longest registered font name a style can hold, terminator included. */
#define NYA_UI_FONT_NAME_MAX 32

/** Widgets whose transitions are remembered, by id modulo this. */
#define NYA_UI_ANIMATIONS_MAX 64

/** How far a panel slides in from while it appears, and how long it must have been gone to appear again. */
#define NYA_UI_APPEAR_OFFSET 12.0F
#define NYA_UI_APPEAR_GAP_S  0.25

/** A colour picker's saturation and value field height, and the thickness of its hue and alpha bars, at scale 1. */
#define NYA_UI_PICKER_FIELD 96.0F
#define NYA_UI_PICKER_BAR   12.0F

/** Longest texture handle a skin can hold, terminator included. */
#define NYA_UI_SKIN_TEXTURE_MAX 64

/** The largest buffer a text field edits, terminator included. */
#define NYA_UI_TEXT_INPUT_MAX 256

/** How long a direction is held before it repeats, and how often it repeats after that, in seconds. */
#define NYA_UI_REPEAT_DELAY_S    0.35F
#define NYA_UI_REPEAT_INTERVAL_S 0.08F

/** How long a newly focused widget takes to settle from its pop. */
#define NYA_UI_POP_S 0.15F

/** How long a caret stays on and then off, in seconds. */
#define NYA_UI_CARET_BLINK_S 0.5F

/** Pixels at scale 1: a scroll per wheel notch, a scrolling panel's bar, and the focus mark along a widget's edge. */
#define NYA_UI_SCROLL_STEP 40.0F
#define NYA_UI_SCROLLBAR   4.0F
#define NYA_UI_FOCUS_BAR   3.0F

/** A derived scale moves in steps this size and never drops under the smallest. */
#define NYA_UI_SCALE_STEP 0.25F
#define NYA_UI_SCALE_MIN  0.5F

/* What a zeroed style field becomes. A quiet dark card, flat fills, one accent, and no outline, shadow or pop. */

#define NYA_UI_REFERENCE_HEIGHT 720.0F
#define NYA_UI_BODY_SIZE        18.0F
#define NYA_UI_SMALL_SIZE       14.0F
#define NYA_UI_TITLE_SIZE       30.0F
#define NYA_UI_MARGIN           16.0F
#define NYA_UI_PADDING          8.0F
#define NYA_UI_SPACING          6.0F
#define NYA_UI_RADIUS           4.0F
#define NYA_UI_OUTLINE          0.0F
#define NYA_UI_DEPTH            0.0F
#define NYA_UI_POP              0.0F

#define NYA_UI_SCRIM            ((NYA_Color){ 0.0F, 0.0F, 0.0F, 0.45F })
#define NYA_UI_PANEL            ((NYA_Color){ 0.10F, 0.11F, 0.13F, 0.92F })
#define NYA_UI_INK              ((NYA_Color){ 0.05F, 0.05F, 0.06F, 1.0F })
#define NYA_UI_TRACK            ((NYA_Color){ 0.06F, 0.07F, 0.08F, 1.0F })
#define NYA_UI_ACCENT           ((NYA_Color){ 0.33F, 0.60F, 0.90F, 1.0F })
#define NYA_UI_TEXT_DIM         ((NYA_Color){ 0.58F, 0.60F, 0.64F, 1.0F })
#define NYA_UI_BUTTON           ((NYA_Color){ 0.16F, 0.17F, 0.20F, 1.0F })
#define NYA_UI_BUTTON_FOCUSED   ((NYA_Color){ 0.20F, 0.25F, 0.32F, 1.0F })
#define NYA_UI_BUTTON_PRESSED   ((NYA_Color){ 0.13F, 0.17F, 0.23F, 1.0F })
#define NYA_UI_BUTTON_DISABLED  ((NYA_Color){ 0.13F, 0.14F, 0.16F, 1.0F })
#define NYA_UI_TEXT             ((NYA_Color){ 0.90F, 0.91F, 0.93F, 1.0F })
#define NYA_UI_TEXT_FOCUSED     ((NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F })
#define NYA_UI_TEXT_PRESSED     ((NYA_Color){ 0.82F, 0.86F, 0.94F, 1.0F })
#define NYA_UI_TEXT_DISABLED    ((NYA_Color){ 0.44F, 0.45F, 0.48F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_UI            NYA_UI;
typedef struct NYA_UISize        NYA_UISize;
typedef struct NYA_UIStateColors NYA_UIStateColors;
typedef struct NYA_UISkin        NYA_UISkin;
typedef struct NYA_UIStateSkins  NYA_UIStateSkins;
typedef struct NYA_UIStyle       NYA_UIStyle;
typedef struct NYA_UIPanel       NYA_UIPanel;

/** What a pass over the UI does. See the file header for why there are two. */
typedef enum NYA_UIPass {
    /** From on_update: reads this tick's input, moves focus and values, and draws nothing. */
    NYA_UI_PASS_INPUT = 0,

    /** From on_render: draws, and every widget returns false. */
    NYA_UI_PASS_DRAW,

    NYA_UI_PASS_COUNT,
} NYA_UIPass;

/** Where a top level panel sits in the window's safe area. Nested panels are placed by their container instead. */
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

/** How a container stacks its children. */
typedef enum NYA_UIDirection {
    NYA_UI_DIRECTION_COLUMN = 0,
    NYA_UI_DIRECTION_ROW,

    NYA_UI_DIRECTION_COUNT,
} NYA_UIDirection;

/** How a child that does not fill sits across its container's direction. */
typedef enum NYA_UIAlign {
    NYA_UI_ALIGN_START = 0,
    NYA_UI_ALIGN_CENTER,
    NYA_UI_ALIGN_END,

    NYA_UI_ALIGN_COUNT,
} NYA_UIAlign;

typedef enum NYA_UISizeKind {
    /** Along a container's direction, its `children` size, which itself defaults to fitting. Across it, filling or fitting as the child does. */
    NYA_UI_SIZE_AUTO = 0,

    /** What the content measures. */
    NYA_UI_SIZE_FIT,

    /** `value` pixels at scale 1. */
    NYA_UI_SIZE_FIXED,

    /** A share of what fixed and fitting siblings leave, by the weight `value`. A top level panel takes `value` of the safe area. */
    NYA_UI_SIZE_GROW,

    NYA_UI_SIZE_COUNT,
} NYA_UISizeKind;

/** What a label does with text wider than the room it has. */
// @reflect
typedef enum NYA_UIOverflow {
    /** A panel keeps the enclosing one's, and a style means VISIBLE. */
    NYA_UI_OVERFLOW_INHERIT = 0,

    /** Runs past the edge, and a container that fits its content grows to hold it. */
    NYA_UI_OVERFLOW_VISIBLE,

    /** Breaks into lines at the room's width. */
    NYA_UI_OVERFLOW_WRAP,

    /** Drops to the largest whole point size that fits on one line. Each size is a glyph atlas of its own. */
    NYA_UI_OVERFLOW_SHRINK,

    NYA_UI_OVERFLOW_COUNT,
} NYA_UIOverflow;

/** The typography scale. */
typedef enum NYA_UIText {
    /** A panel keeps the enclosing one's, which at the top level is BODY. */
    NYA_UI_TEXT_INHERIT = 0,
    NYA_UI_TEXT_BODY,
    NYA_UI_TEXT_SMALL,
    NYA_UI_TEXT_TITLE,

    NYA_UI_TEXT_COUNT,
} NYA_UIText;

/** How big something is along one axis. `min` and `max` bound any kind, in pixels at scale 1; a zero `max` is no bound. */
struct NYA_UISize {
    NYA_UISizeKind kind;
    f32            value;
    f32            min;
    f32            max;
};

#define nya_ui_fit()         ((NYA_UISize){ .kind = NYA_UI_SIZE_FIT })
#define nya_ui_fixed(pixels) ((NYA_UISize){ .kind = NYA_UI_SIZE_FIXED, .value = (f32)(pixels) })
#define nya_ui_grow(weight)  ((NYA_UISize){ .kind = NYA_UI_SIZE_GROW, .value = (f32)(weight) })

/** A colour for each state a widget can be in. */
// @reflect
struct NYA_UIStateColors {
    NYA_Color normal;
    NYA_Color focused;
    NYA_Color pressed;
    NYA_Color disabled;
};

/**
 * How one part of the UI draws: flat, or a nine-slice cut from a texture. Zeroed, it is flat, so a style that names no
 * texture looks as it did.
 * */
// @reflect
struct NYA_UISkin {
    /** A texture asset handle, loaded on first use. Empty cuts the region from the panel skin's texture, or draws flat without a region. */
    char texture[NYA_UI_SKIN_TEXTURE_MAX];

    /** The region of a sheet the slice is cut from, in its pixels. A zero size is the whole texture. */
    f32 source_x;
    f32 source_y;
    f32 source_width;
    f32 source_height;

    /** Border insets in source pixels, scaled with the UI and snapped to whole pixels. A panel's padding defaults to them. */
    f32 left;
    f32 right;
    f32 top;
    f32 bottom;

    /** Repeats the edges and centre instead of stretching them. */
    b8 tile;

    /** Leaves the centre undrawn. */
    b8 hollow;

    /** Multiplies the texture. All four channels zero takes the flat colour of what it draws, so a white sheet follows the style. */
    NYA_Color tint;
};

/** A skin for each state a widget can be in. A state without a texture takes the normal one's, tinted with its own tint when it has one. */
// @reflect
struct NYA_UIStateSkins {
    NYA_UISkin normal;
    NYA_UISkin focused;
    NYA_UISkin pressed;
    NYA_UISkin disabled;
};

/**
 * The look of every widget in a window. A zeroed field, or a colour with all four channels zero, takes its
 * NYA_UI_* default, so a zeroed style is the default look. Sizes are pixels at scale 1.
 * */
// @reflect
struct NYA_UIStyle {
    /**
     * Registered font names: the face body and small text are cut from, and the one titles are. Empty uses
     * nya_font_default's face, and the body's for titles. A face registered as a distance field stays one at every
     * size the scale asks for.
     * */
    char font[NYA_UI_FONT_NAME_MAX];
    char title_font[NYA_UI_FONT_NAME_MAX];

    /** The typography scale: labels and widgets, secondary text, and titles. */
    f32 body_size;
    f32 small_size;
    f32 title_size;

    /** Multiplies every size. Zero derives it: the window's height against `reference_height`, at least the display's scale. */
    f32 scale;
    f32 reference_height;

    /** Between the window's safe area and a top level panel. */
    f32 margin;

    /** Inside a panel's edge, and around a button's label. */
    f32 padding;

    /** Between children. */
    f32 spacing;

    f32 radius;

    /** The ink line around panels and widgets. None by default. */
    f32 outline;

    /** How far an ink shadow drops under a panel or widget, and so how far a pressed one sinks. None by default. */
    f32 depth;

    /** How much a newly focused widget grows before settling. None by default. */
    f32 pop;

    /** A button, slider, toggle, selectable or field's height. Zero is the text's line height plus padding. */
    f32 item_height;

    /** What labels do with text too wide for them, unless a panel says otherwise. */
    NYA_UIOverflow overflow;

    /** Seconds focus and press colours take to change, and a panel takes to slide in. Zero is instant and costs nothing. */
    f32 transition_s;
    f32 appear_s;

    /** The curve both follow. */
    NYA_EaseType easing;

    NYA_Color scrim;
    NYA_Color panel;

    /** Outlines and shadows. */
    NYA_Color ink;

    /** The empty part of sliders, toggles and fields. */
    NYA_Color track;

    /** The focus mark, and the filled part of sliders and toggles. */
    NYA_Color accent;

    /** Secondary labels. */
    NYA_Color text_dim;

    /** Widget bodies, and the text on them and in labels. */
    NYA_UIStateColors button;
    NYA_UIStateColors text;

    /** Nine-slices in place of the flat shapes: panels, widget bodies, the tracks of sliders, toggles and fields, and knobs. */
    NYA_UISkin       panel_skin;
    NYA_UIStateSkins button_skin;
    NYA_UISkin       track_skin;
    NYA_UISkin       knob_skin;
};

/** A container. At the top level it is anchored in the window; nested, its container places it like any child. */
struct NYA_UIPanel {
    NYA_UIAnchor anchor;

    /** Pixels in from the anchored edges of the safe area. Ignored on the axis an anchor centres. */
    f32x2 offset;

    /** AUTO fits a top level panel's content, which never grows past the safe area and scrolls instead. */
    NYA_UISize width;
    NYA_UISize height;

    NYA_UIDirection direction;

    /** A child's size along `direction` when nya_ui_size did not give one. AUTO fits. */
    NYA_UISize children;

    NYA_UIAlign align;

    /** Between children, and inside the edge. Zero takes the style's spacing, and its padding unless frameless. */
    f32 gap;
    f32 padding;

    /** What labels inside do with text too wide for them. */
    NYA_UIOverflow overflow;

    /** The text size of everything inside. */
    NYA_UIText text;

    /** Drawn centred at the top at the title size. Optional. */
    NYA_ConstCString title;

    /** Overrides the style's panel colour. All four channels zero keeps it. */
    NYA_Color fill;

    /** No background, outline or padding: a plain column or row. */
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
 * Opens a container. `id` names it and scopes the ids of what is inside; null leaves it unnamed, remembered by its
 * place in its container.
 *
 * False when the container table is full with ones this pass already opened; skip the contents and the end.
 * */
NYA_API b8   nya_ui_panel_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel) __attr_no_discard;
NYA_API void nya_ui_panel_end(NYA_UI* ui);

/** The size of the next child, widget or container, along its container's direction. */
NYA_API void nya_ui_size(NYA_UI* ui, NYA_UISize size);

/**
 * Takes room in the layout and returns it, in window pixels, for drawing into during the draw pass. `width` and
 * `height` are pixels at scale 1; a zero across the container's direction fills it.
 * */
NYA_API NYA_Rectf nya_ui_space(NYA_UI* ui, f32 width, f32 height);

/** Dims the whole window in the style's scrim colour, under whatever is drawn after it. */
NYA_API void nya_ui_scrim(NYA_UI* ui);

/*
 * ─────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────
 */

/** Text at the container's size, in the style's text colour or in `color`. See NYA_UIOverflow for text too wide. */
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

/**
 * `label`, then a field showing the UTF-8 in `buffer`, which holds at most `capacity` bytes with its terminator.
 * Activating it starts typing: text goes in at the caret, left and right move it, backspace and delete remove a
 * character, home and end jump. Return, cancel, or a click elsewhere stop. True when the text changed.
 * */
NYA_API b8 nya_ui_text_input(NYA_UI* ui, NYA_ConstCString label, char* buffer, u32 capacity);

/**
 * `label` and a swatch, a field of saturation across and value down with a hue bar beside it, an alpha bar under it,
 * and a hex field under that. Dragging sets the part under the pointer; focused, left and right turn the hue. The hex
 * field takes six or eight digits, with or without a '#'. True when `*color` changed.
 * */
NYA_API b8 nya_ui_color_picker(NYA_UI* ui, NYA_ConstCString label, NYA_Color* color);

/*
 * ─────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────
 */

/** Widgets until the matching end draw in their disabled colours, take no focus and never act. Nests. */
NYA_API void nya_ui_disabled_begin(NYA_UI* ui);
NYA_API void nya_ui_disabled_end(NYA_UI* ui);

/** Whether cancel was pressed this pass. Always false in a draw pass, and when the cancel only stopped typing. */
NYA_API b8 nya_ui_cancelled(const NYA_UI* ui) __attr_no_discard;

/**
 * Whether a field had the keyboard in the last input pass, including the pass that stopped it. A layer reading its
 * own keys after the UI skips them then, so escape out of a field does not also close the menu.
 * */
NYA_API b8 nya_ui_typing(const NYA_Window* window) __attr_no_discard;

/** Moves focus to the first widget of the next pass, stops typing and drops any press. For a menu that opens fresh. */
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

/**
 * Draws what follows with `style`, zeroes filled in, until the matching pop. At most NYA_UI_STYLE_DEPTH_MAX deep, and
 * balanced by the end of the pass. The scale stays the window's.
 * */
NYA_API void nya_ui_style_push(NYA_UI* ui, NYA_UIStyle style);
NYA_API void nya_ui_style_pop(NYA_UI* ui);

/** What sizes were multiplied by in `window`'s last pass, for drawing custom content at the same scale. */
NYA_API f32 nya_ui_scale(const NYA_Window* window) __attr_no_discard;
