/**
 * @file ui_present.h
 *
 * The seam between a widget and whatever puts it on a screen. A `nya_ui_*` call decides what a widget *is* this
 * pass — where it sits, whether it has focus, what its value became — and then hands one NYA_UIWidgetDraw to the
 * window's presenter. The presenter decides what that looks like. No file under `ui/` above this one names a
 * drawing primitive, a font or a colour of the backend's, which is what lets the same component code run on a GPU,
 * in a terminal and, later, against a DOM.
 *
 * Functions
 *
 *   nya_ui_presenter_set, nya_ui_presenter_get   the presenter a window's passes go through
 *   nya_ui_presenter_shape                       the default: shapes and glyphs through render2d
 *   nya_ui_look_scale                            the default pixel arithmetic, for a presenter that wants it
 *   nya_ui_recorder_init, nya_ui_recorder_reset  the recording presenter's caller owned tables
 *   nya_ui_recorder_presenter                    a presenter writing into that recorder
 *   nya_ui_recorder_count, nya_ui_recorder_at    what one pass declared, in declaration order
 *   nya_ui_recorder_write                        the same as one line of text per widget
 *
 * ```c
 * // a UI pass nothing draws, from a program with no GPU.
 * static NYA_UIRecorder recorder;
 * nya_ui_recorder_init(&recorder, (f32x2){ 8.0F, 16.0F });
 * nya_ui_presenter_set(window, nya_ui_recorder_presenter(&recorder));
 *
 * nya_ui_recorder_reset(&recorder);
 * menu(window, NYA_UI_PASS_DRAW);
 *
 * for (u32 i = 0; i < nya_ui_recorder_count(&recorder); i++) {
 *     const NYA_UIWidgetDraw* widget = nya_ui_recorder_at(&recorder, i);
 *     nya_log_info("%s %s", nya_ui_widget_kind_name(widget->kind), widget->label);
 * }
 * ```
 *
 * ## The dispatch shape, and why
 *
 * One indirect call per widget, through the function pointers on NYA_UIPresenter. Never one per glyph, per shape or
 * per colour: a whole widget, its label included, goes out in a single NYA_UIWidgetDraw, so a menu of forty widgets
 * costs forty indirect calls a pass and the backend is free to batch everything under them. A presenter that wants
 * per-glyph control gets it inside its own `draw`, where it is a direct call.
 *
 * Drawing is one function rather than one per widget because a backend must answer for *every* widget: a null
 * pointer in a table of twenty would be a crash the day a widget is added, while one switch over NYA_UIWidgetKind
 * is a `-Wswitch` error at the build that adds it. It is the same bargain `render_features.h` makes with the
 * terminal backend, and for the same reason.
 *
 * ## What the layout still asks, and why it has to
 *
 * Measurement. A terminal measures in cells and a GPU in pixels, and the layout cannot be written twice, so
 * `measure` and `measure_bytes` are the presenter's and every size in the UI comes back out of them. `look_build`
 * is the same idea for the style: the presenter turns a NYA_UIStyle and a scale into the pixel numbers the layout
 * adds up, so a cell backend can round them to whole cells and the layout never learns that it did.
 *
 * ## What a widget can still tell about its backend
 *
 * Its own measurements, and that is the intended leak: a label that fits on a GPU wraps in a terminal because the
 * cell is wider than the glyph, and that is the answer the caller wants. What it cannot see is which presenter is
 * installed, what a pixel is, whether text is a texture or a cell, or whether anything was drawn at all.
 *
 * Two things do leak today and are worth naming. `NYA_UIWidgetDraw` carries the *interactive* rectangle of a value
 * widget — a slider's track, a field's box, a picker's three bars — because the pointer is read against it in the
 * input pass and the presenter must paint exactly where the input reads; a DOM presenter will ignore it for hit
 * testing and use it only for placement. And NYA_UISkin and NYA_UIIcon name a texture asset, which a backend
 * without a sampler answers by drawing the flat colour underneath instead.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/ui/ui.h"

typedef struct NYA_Window NYA_Window;


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Widgets one recorded pass keeps. A pass that declares more is refused the extra ones, as the widget table is. */
#ifndef NYA_UI_RECORD_MAX
#define NYA_UI_RECORD_MAX 256
#endif

/** Bytes of label and value text one recorded pass keeps. Text is copied, since a caller's string outlives nothing. */
#ifndef NYA_UI_RECORD_TEXT_MAX
#define NYA_UI_RECORD_TEXT_MAX 8192
#endif

/** What a recorder measures with when its caller names no cell: the terminal's, so a dump reads like a TUI. */
#define NYA_UI_RECORD_CELL ((f32x2){ 8.0F, 16.0F })


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_UILook       NYA_UILook;
typedef struct NYA_UIFieldDraw  NYA_UIFieldDraw;
typedef struct NYA_UIWidgetDraw NYA_UIWidgetDraw;
typedef struct NYA_UIPresenter  NYA_UIPresenter;
typedef struct NYA_UIRecorder   NYA_UIRecorder;

/**
 * A style with every size multiplied by the pass's scale and rounded the way the backend wants them, plus the line
 * height of each text role. The layout adds these up and knows nothing else about how big anything is.
 * */
struct NYA_UILook {
    NYA_UIStyle style;

    f32 margin;
    f32 padding;
    f32 spacing;
    f32 radius;
    f32 outline;
    f32 depth;
    f32 pop;
    f32 focus_bar;
    f32 item_height;

    /**
     * A title bar's height, which is the title's line height with room around it rather than the line
     * height itself: a bar exactly as tall as its text has the text touching both edges and the chrome
     * squares filling it corner to corner, which is what made the bar read as a row of buttons.
     *
     * Zero until the title face is usable, like the line heights it is worked out from.
     * */
    f32 title_bar;

    /** Indexed by NYA_UIText, INHERIT holding the body's. Zero while the face behind a role is not usable yet. */
    f32 line_heights[NYA_UI_TEXT_COUNT];
};

/** Where one widget stands this pass. What a presenter is allowed to know about state, and all of it. */
typedef struct NYA_UIWidgetState NYA_UIWidgetState;

struct NYA_UIWidgetState {
    u64 id;

    b8 disabled;
    b8 focused;
    b8 held;
    b8 activated;

    /** How focused and how pressed it looks, eased, and how far through its focus pop and its click bounce it is. */
    f32 focus;
    f32 press;
    f32 pop;
    f32 bounce;
};

/** The X, chevron, hamburger and corner grip a window's chrome is drawn from. */
typedef enum NYA_UIMark {
    NYA_UI_MARK_CLOSE = 0,
    NYA_UI_MARK_COLLAPSED,
    NYA_UI_MARK_EXPANDED,
    NYA_UI_MARK_MENU,
    NYA_UI_MARK_GRIP,

    NYA_UI_MARK_COUNT,
} NYA_UIMark;

/**
 * One line of editable text inside a widget: the box the pointer is read against, the caller's buffer, the caret
 * and the selection's other end in bytes, and the IME's unfinished text with the run it has selected. `shift` is
 * how far the line is scrolled so the caret stays inside the box.
 *
 * Its own struct because two widgets have one: a text input row, and a colour picker's hex field.
 * */
struct NYA_UIFieldDraw {
    NYA_Rectf        box;
    NYA_ConstCString buffer;
    NYA_ConstCString composing;
    u32              caret;
    u32              select;
    u32              composing_from;
    u32              composing_to;
    f32              shift;
    b8               editing;
};

/**
 * Everything the UI can ask to be drawn. A presenter answers every one of them; there is no opting out, because a
 * widget that only works on some backends is not a widget, it is a platform detail with a label on it.
 * */
typedef enum NYA_UIWidgetKind {
    /** The whole window dimmed, under a modal. */
    NYA_UI_WIDGET_SCRIM = 0,

    /** A container's frame and title. Its children arrive after it, in declaration order. */
    NYA_UI_WIDGET_PANEL,

    NYA_UI_WIDGET_LABEL,
    NYA_UI_WIDGET_BUTTON,
    NYA_UI_WIDGET_SELECTABLE,
    NYA_UI_WIDGET_TOGGLE,
    NYA_UI_WIDGET_SLIDER,
    NYA_UI_WIDGET_RADIO,
    NYA_UI_WIDGET_DROPDOWN,

    /** One line of editable text: the box, what is in it, the selection, the IME's guess and the caret. */
    NYA_UI_WIDGET_FIELD,

    NYA_UI_WIDGET_COLOR_PICKER,
    NYA_UI_WIDGET_CHART,
    NYA_UI_WIDGET_ICON,

    /** A section's fold header, one square button of a window's title bar, and the grip that resizes it. */
    NYA_UI_WIDGET_SECTION,
    NYA_UI_WIDGET_CHROME,
    NYA_UI_WIDGET_GRIP,

    /** A scrolling container's thumb, a table's header rule and stripe, and the accent under a chosen tab. */
    NYA_UI_WIDGET_SCROLLBAR,
    NYA_UI_WIDGET_RULE,
    NYA_UI_WIDGET_STRIPE,
    NYA_UI_WIDGET_UNDERLINE,

    NYA_UI_WIDGET_KIND_COUNT,
} NYA_UIWidgetKind;

/**
 * One widget, whole. Everything a backend needs to draw it and nothing about how: no colours of the backend's, no
 * font, no shape. Zeroed fields are the ones this `kind` does not use, so a caller fills what it means and the rest
 * reads as absent.
 * */
struct NYA_UIWidgetDraw {
    NYA_UIWidgetKind kind;

    /** Where the whole widget was placed, in window pixels. */
    NYA_Rectf rect;

    NYA_UIWidgetState state;

    /** The text role everything in this widget is written at. */
    NYA_UIText text;

    /** The widget's own words. Never null: "" when it has none. */
    NYA_ConstCString label;

    /** What the caller asked this widget's text or fill to be. All four channels zero means the style decides. */
    NYA_Color color;

    /** The product of the opacity groups open around it. One when there are none. */
    f32 opacity;

    /** What the container it sits in cuts drawing to, and whether that is the whole window. */
    NYA_Rectf clip;
    b8        clip_whole;

    union {
        /** LABEL: how wide it may run and what it does when it does not fit. */
        struct {
            f32            room;
            NYA_UIOverflow overflow;
            NYA_UIAlign    align;
        } as_label;

        /** PANEL: the caller's options, the title's measured width, and where the content starts inside the frame. */
        struct {
            const NYA_UIPanel* options;
            f32                title_width;
            f32x2              inset;

            /** The title bar's height, so the title is centred in it rather than sitting on its top edge. */
            f32 bar;

            /** Room the caller's chrome takes at each end of the bar; the title stays between them. */
            f32x2 title_room;
        } as_panel;

        /** SELECTABLE, TOGGLE, RADIO: whether it is the chosen one. */
        struct {
            b8 on;
        } as_choice;

        /** SLIDER: the track the pointer is read against, and how far along it the value sits. */
        struct {
            NYA_Rectf track;
            f32       t;
        } as_slider;

        /** DROPDOWN: the chosen option's words, and whether its list is showing. */
        struct {
            NYA_ConstCString shown;
            b8               open;
        } as_dropdown;

        /** FIELD: a labelled row whose right hand side is one line of editable text. */
        struct {
            NYA_UIFieldDraw field;
        } as_field;

        /** COLOR_PICKER: the four parts the pointer is read against, the colour in both forms, and the hex field. */
        struct {
            NYA_Rectf    plane;
            NYA_Rectf    hue;
            NYA_Rectf    alpha;
            NYA_Rectf    swatch;
            NYA_ColorHSV hsv;
            NYA_Color    value;

            NYA_UIFieldDraw field;
        } as_picker;

        /** CHART: the caller's values, read during the call and never again. */
        struct {
            const NYA_UIChart* chart;
        } as_chart;

        /** ICON: the caller's region of a sheet. */
        struct {
            const NYA_UIIcon* icon;
        } as_icon;

        /** SECTION, CHROME, GRIP: the mark drawn beside or instead of the label. */
        struct {
            NYA_UIMark mark;

            /** CHROME only: whether the body under the mark is worth drawing at all. */
            b8 body;
        } as_mark;
    };
};

/**
 * One backend. `state` is the presenter's own and is handed back to every call; nothing in the UI reads it.
 *
 * Every pointer is required. A presenter with a null one is refused at `nya_ui_presenter_set` rather than at the
 * first pass that happens to use it.
 * */
struct NYA_UIPresenter {
    /** What it is, for assertions and the debug overlay. */
    NYA_ConstCString name;

    void* state;

    /*
     * THE LOOK. The UI mirrors its style stack onto the presenter: `build` fills the numbers the layout adds up and
     * the presenter keeps whatever else it needs at that depth, `use` says which depth the calls below now mean.
     */

    void (*look_build)(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
    void (*look_use)(void* state, u32 depth);

    /*
     * MEASUREMENT. In window pixels, at the current look. This is what a terminal answers in cells and a GPU in
     * glyph advances, and the only reason the layout can be written once.
     */

    /** `text` at role `role`, within `room` pixels and folded or shrunk by `overflow`. Height is one line unless it wrapped. */
    f32x2 (*measure)(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);

    /** The first `bytes` of `text`, for a caret and a selection. Never past the string's own width. */
    f32 (*measure_bytes)(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);

    /*
     * THE SURFACE. Clipping and ordering, which are the backend's and not the widget's.
     */

    /** Cuts drawing to `clip`. `whole` when that is the entire window, which is the backend's chance to stop clipping. */
    void (*clip_set)(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);

    /** Where drawing lands in the back to front order, so a raised panel covers one declared before it. */
    s32 (*layer_get)(void* state, NYA_Window* window);
    void (*layer_set)(void* state, NYA_Window* window, s32 layer);

    /*
     * DRAWING. One call, one widget. See the file header for why it is one call.
     */

    void (*draw)(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);
};

/**
 * The recording presenter's tables, which the caller owns and which outlive a pass. Fixed, like everything else in
 * the module: a recorded pass allocates nothing.
 * */
struct NYA_UIRecorder {
    /** Filled by nya_ui_recorder_init; passed to nya_ui_presenter_set as nya_ui_recorder_presenter(recorder). */
    NYA_UIPresenter presenter;

    /** One cell, in pixels. Every measurement is a multiple of it, so a recorded layout is exact. */
    f32x2 cell;

    NYA_UIWidgetDraw widgets[NYA_UI_RECORD_MAX];
    u32              count;

    /** How many widgets the pass wanted, which is past `count` when the table filled. */
    u32 wanted;

    /** Copied text, since a label the caller built on its stack is gone by the time anything reads this. */
    char text[NYA_UI_RECORD_TEXT_MAX];
    u32  text_used;

    /** The style stack, mirrored, and which depth is current. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32        depth;

    /** What clip_set and layer_set were last told, so a test can read them back. */
    NYA_Rectf clip;
    s32       layer;
};


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * THE SEAM
 * ─────────────────────────────────────────────────────────
 */

/**
 * The presenter `window`'s passes go through, from the next one on. `presenter` outlives every pass that uses it;
 * null puts the shape presenter back.
 *
 * Never call it inside an open pass: a pass measures with the presenter it began with, and swapping halfway would
 * lay the second half of a menu out against the first half's numbers.
 * */
NYA_API void nya_ui_presenter_set(NYA_Window* window, const NYA_UIPresenter* presenter);

NYA_API const NYA_UIPresenter* nya_ui_presenter_get(const NYA_Window* window) __attr_no_discard;

/** Shapes and glyphs through render2d and the font registry: what every window uses until told otherwise. */
NYA_API const NYA_UIPresenter* nya_ui_presenter_shape(void) __attr_no_discard;

/** The kind's name, for logs and assertions. */
NYA_API NYA_ConstCString nya_ui_widget_kind_name(NYA_UIWidgetKind kind) __attr_no_discard;

/**
 * The default `look_build` arithmetic: every size multiplied by `scale` and rounded to a whole pixel, and the line
 * heights left at zero. A presenter calls this and then writes the line heights its own measurement gives, plus
 * whatever rounding its pixels need.
 * */
NYA_API void nya_ui_look_scale(const NYA_UIStyle* style, f32 scale, NYA_UILook* out);

/*
 * ─────────────────────────────────────────────────────────
 * RECORDING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Prepares `recorder` and the presenter inside it. `cell` is what text is measured in, a zero cell taking
 * NYA_UI_RECORD_CELL; a monospace cell is the point, since it makes a recorded layout exact and comparable rather
 * than dependent on a face that may not be loaded.
 * */
NYA_API void nya_ui_recorder_init(NYA_UIRecorder* recorder, f32x2 cell);

/**
 * Throws everything away, the presenter inside it included, so a window still pointing at a recorder whose storage
 * is gone asserts at the next pass rather than writing into it.
 * */
NYA_API void nya_ui_recorder_deinit(NYA_UIRecorder* recorder);

/** Throws the last pass away. Called before each pass; a recorder that is never reset fills and refuses the rest. */
NYA_API void nya_ui_recorder_reset(NYA_UIRecorder* recorder);

NYA_API const NYA_UIPresenter* nya_ui_recorder_presenter(NYA_UIRecorder* recorder) __attr_no_discard;

NYA_API u32                     nya_ui_recorder_count(const NYA_UIRecorder* recorder) __attr_no_discard;
NYA_API const NYA_UIWidgetDraw* nya_ui_recorder_at(const NYA_UIRecorder* recorder, u32 index) __attr_no_discard;

/** The first recorded widget of `kind` whose label is `label`, or null. `label` null matches any. */
NYA_API const NYA_UIWidgetDraw* nya_ui_recorder_find(const NYA_UIRecorder* recorder, NYA_UIWidgetKind kind, NYA_ConstCString label) __attr_no_discard;

/**
 * The pass as text, one line per widget: kind, rectangle, state and label. Written into `out` and terminated, at
 * most `capacity` bytes including the terminator. How many bytes it wrote.
 * */
NYA_API u32 nya_ui_recorder_write(const NYA_UIRecorder* recorder, char* out, u32 capacity);
