/**
 * @file ui_internal.h
 *
 * What the ui_*.c files share: the per window state, the open pass's scratch, and every internal function. Nothing
 * here is public; ui.h is the module's contract. See ui.h for what the module is and why it is shaped this way.
 *
 * The state itself lives in ui.c, which nyangine.c includes before the rest of the module for that reason.
 * */
#pragma once

#include "nyangine/base/base_perf.h"
#include "nyangine/core/core_input.h"
#include "nyangine/core/core_keys.h"
#include "nyangine/core/core_window.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/renderer/render_font.h"
#include "nyangine/ui/ui.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A style with its sizes multiplied by the pass's scale, in whole pixels, and its fonts resolved. */
typedef struct {
    NYA_UIStyle style;

    f32 margin;
    f32 padding;
    f32 spacing;
    f32 radius;
    f32 outline;
    f32 depth;
    f32 pop;
    f32 item_height;

    /** Indexed by NYA_UIText, INHERIT holding the body's. */
    NYA_Font fonts[NYA_UI_TEXT_COUNT];
    f32      line_heights[NYA_UI_TEXT_COUNT];
} _NYA_UILook;

/** One open container. Axis 0 is across the window and 1 down it. */
typedef struct {
    /** The content's top left in window pixels, scroll included, and its size, zero on an axis not measured yet. */
    f32x2 origin;
    f32x2 extent;

    /** The most the content may take on each axis, known before measuring: what wrapping fits to. */
    f32x2 room;

    /** The axis children stack along: 1 in a column, 0 in a row. */
    u32 main;

    f32            gap;
    NYA_UISize     children;
    NYA_UIAlign    align;
    NYA_UIOverflow overflow;
    NYA_UIText     text;

    /** This pass so far: the length along `main`, gaps included, the widest child across, and the children. */
    f32 used;
    f32 across;
    u32 count;

    /** This pass so far: what non-growing children took, the weight growing ones asked for, and the weight placed. */
    f32 fixed;
    f32 grow;
    f32 grow_placed;

    /** From the previous pass: the length growing children share, and their total weight. */
    f32 grow_space;
    f32 grow_total;

    u64 key;
    u64 scope;

    /** The first widget of the outermost open row, which every focusable cell in it shares. U32_MAX outside rows. */
    u32 group;

    /**
     * A table row's column widths in pixels at scale 1, null outside one. Each child takes the width of the column
     * its index names, which is what keeps a table's cells lined up without the caller sizing every one.
     * */
    const f32* columns;
    u32        column_count;

    /** Whether the table these columns come from tints every other row. */
    b8 striped;

    /** Into _NYA_UISystem.panels, or U32_MAX for the root. */
    u32         panel;
    NYA_UIPanel options;
    NYA_Rectf   bounds;

    /** The padding and outline before the content on each axis, and after it. */
    f32x2 before;
    f32x2 after;

    f32   header;
    f32   title_width;
    f32x2 scroll;

    /** What the pointer can reach and drawing shows. The window, until a scrolling container narrows it. */
    NYA_Rectf clip;

    /** Whether the content is longer than the container on each axis, and whether this pass set a scissor for it. */
    b8 scrolls[2];
    b8 clipping;

    /** Lays out without drawing: a container with no measurements yet, or a font still loading. Inherited inward. */
    b8 hidden;
} _NYA_UILayout;

typedef struct {
    /** Zero is a free slot. */
    u64 id;

    /** The pass that last opened it, so a full table only refuses containers that pass is still using. */
    u64 pass;

    /** What the last pass measured: the natural outer size, the content's size on both axes, and its children. */
    f32x2 size;
    b8    measured;
    f32x2 content;
    f32   fixed;
    f32   grow;
    u32   count;

    /** How far the content is scrolled on each axis, in pixels. */
    f32x2 scroll;

    /** How far a draggable panel has been moved from where its anchor puts it, in pixels. */
    f32x2 drag;

    /** Wall clock seconds when it last began showing, and when it was last opened. */
    f64 shown_s;
    f64 seen_s;
} _NYA_UIPanelState;

/** A widget's standing in the current pass. */
typedef struct {
    u64 id;
    b8  refused;
    b8  disabled;
    b8  focused;
    b8  held;
    b8  activated;

    /** How focused and how pressed it looks, eased: the flags above, or on their way there. */
    f32 focus;
    f32 press;
} _NYA_UIWidget;

/** Where one widget's transitions stand, and when they were last stepped. */
typedef struct {
    u64 id;
    f64 time_s;
    f32 focus;
    f32 press;
} _NYA_UIAnimation;

/** What persists per window. */
struct NYA_UI {
    b8               claimed;
    NYA_WindowHandle handle;
    NYA_Window*      window;
    NYA_UIPass       pass;

    /** Resolved when set, so every zero is already its default. */
    NYA_UIStyle style;
    f32         scale;

    u64 focus;
    u32 focus_index;
    f64 focus_changed_s;

    /** Whether the keys moved focus since a scrolling container last followed it. */
    b8 reveal;

    /** The widget the left button went down on, while it stays down, whether that press grabbed a slider, and which part of a picker. */
    u64 active;
    b8  dragging;
    u32 grab;

    /** The hue a picker last had, for a grey that has none, and the hex it is typing into. */
    u64  hue_id;
    f32  hue;
    char hex[10];

    /** The field being typed into, the caret's byte offset in it, and whether a field had the keyboard last input pass. */
    u64 editing;
    u32 caret;
    b8  typing;

    /** The other end of the selection, in bytes. Equal to `caret` when nothing is selected. */
    u32 select;

    /** The field the last click in a box landed in and when, so the second click of a double click knows it is one. */
    u64 click_id;
    f64 click_s;

    /** Which choice list is open, and the panel being dragged by its title, both zero for none. */
    u64 open;
    u64 drag_panel;

    /** The widget that was last activated and when, for the bounce. One at a time, since one pointer clicks one. */
    u64 bounce_id;
    f64 bounce_s;

    /** Where in the dragged panel the pointer grabbed it, so it does not jump to the pointer. */
    f32x2 drag_grip;
};

/** What a press is read from: an action, or a raw key when the action is NONE. */
typedef struct {
    NYA_InputAction action;
    NYA_Keycode     key;
} _NYA_UIPress;

enum {
    _NYA_UI_UP = 0,
    _NYA_UI_DOWN,
    _NYA_UI_LEFT,
    _NYA_UI_RIGHT,
    _NYA_UI_CARET_LEFT,
    _NYA_UI_CARET_RIGHT,
    _NYA_UI_BACKSPACE,
    _NYA_UI_DELETE,
    _NYA_UI_PRESS_COUNT,
};

typedef struct {
    NYA_UI windows[NYA_WINDOW_MAX];

    /** The one pass open, whose scratch is everything below. */
    NYA_UI* open;
    u64     pass_serial;
    b8      registered;

    /** Open from begin to end, since the widgets between are the UI's cost. */
    NYA_TraceScope trace;

    /* This pass's input. All false in a draw pass except what feedback reads. */

    b8    confirm;
    b8    cancel;
    b8    confirm_down;
    b8    presses[_NYA_UI_PRESS_COUNT];
    f32x2 pointer;
    b8    pointer_moved;
    b8    pointer_pressed;
    b8    pointer_down;
    b8    pointer_released;

    /** The wheel this pass, down and across. A container that scrolls takes the axis it uses and zeroes it. */
    f32 wheel;
    f32 wheel_x;

    /** Whether the field being typed into was declared this pass, and whether one was typing when the pass began. */
    b8 editing_seen;
    b8 typing_at_begin;

    /**
     * Presses are worked out once per tick for every window, so a second input pass cannot repeat twice as fast.
     * Both counters are one past their value, so a zeroed system has read no tick and holds nothing.
     * */
    u64 press_tick;
    b8  tick_presses[_NYA_UI_PRESS_COUNT];
    u32 repeat_press;
    f32 repeat_s;

    u64 widgets[NYA_UI_WIDGETS_MAX];
    u32 widget_groups[NYA_UI_WIDGETS_MAX];
    b8  widget_horizontal[NYA_UI_WIDGETS_MAX];
    u32 widget_count;
    u32 widget_count_worst;
    u32 focus_found;

    _NYA_UILayout layouts[NYA_UI_DEPTH_MAX];
    u32           depth;

    /** The window's look and the pushed ones on top of it. */
    _NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32         look_depth;

    /** The window's safe area in pixels, inset by the margin. */
    NYA_Rectf safe;

    /** What nya_ui_size set for the next child. */
    NYA_UISize next;
    b8         next_set;

    u32 disabled;

    /**
     * The opacity every colour drawn is multiplied by: the product of the groups open around it. Index 0 is 1, so a
     * pass that never opens a group multiplies by one and costs nothing.
     * */
    f32 opacities[NYA_UI_OPACITY_DEPTH_MAX + 1];
    u32 opacity_depth;

    _NYA_UIPanelState panels[NYA_UI_PANELS_MAX];
    u32               panel_count;

    _NYA_UIAnimation animations[NYA_UI_ANIMATIONS_MAX];
} _NYA_UISystem;


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The one system is `_nya_ui`, defined in ui.c. It is not declared here: the module is internal to one translation
 * unit, so the definition is static, and nyangine.c includes ui.c ahead of the other ui_*.c files for that reason.
 * A declaration here would be a second static object the day a second translation unit includes this header.
 */


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The window's persistent state, reset when its slot now holds a different window. */
NYA_INTERNAL NYA_UI* _nya_ui_context(const NYA_Window* window);

NYA_INTERNAL NYA_UIStyle _nya_ui_style_resolve(NYA_UIStyle style);

/** The explicit scale, or the window's height against the reference height, at least the display's scale, in steps. */
NYA_INTERNAL f32 _nya_ui_scale_derive(const NYA_Window* window, const NYA_UIStyle* style);

NYA_INTERNAL _NYA_UILook        _nya_ui_look_build(const NYA_UIStyle* style, f32 scale);
NYA_INTERNAL const _NYA_UILook* _nya_ui_look(void);

/** `value`, in pixels at scale 1, as whole pixels at the pass's scale. */
NYA_INTERNAL f32 _nya_ui_px(f32 value);

/** Reads confirm, cancel, the pointer, the wheel and the presses. */
NYA_INTERNAL void _nya_ui_input_read(NYA_UI* ui);

/** This tick's presses, and the held one's repeat. Typing repeats the caret keys rather than the directions. */
NYA_INTERNAL void _nya_ui_presses_read(b8 typing);

/** `label` hashed into `scope`. Never zero, which means no widget. */
NYA_INTERNAL u64 _nya_ui_id(u64 scope, NYA_ConstCString label);

/** An unnamed container's key: its place among its container's children. */
NYA_INTERNAL u64 _nya_ui_id_at(u64 key, u32 index);

/** The container slot for `id`: its own, a free one, or the stalest one not opened this pass. U32_MAX when none is left. */
NYA_INTERNAL u32 _nya_ui_panel_claim(u64 id);

NYA_INTERNAL _NYA_UILayout* _nya_ui_layout_push(void);

/** A size spec resolved against what was measured and what is available, bounded by its min and max. */
NYA_INTERNAL f32 _nya_ui_extent(NYA_UISize size, f32 measured, f32 available);

/** The next child's size along its container: nya_ui_size's, else `own` unless AUTO, else the container's `children`. */
NYA_INTERNAL NYA_UISize _nya_ui_next_size(const _NYA_UILayout* layout, NYA_UISize own);

/** The share a growing child of `weight` gets after `placed` weight went before it, rounded so shares add up. Negative before any weights are known. */
NYA_INTERNAL f32 _nya_ui_share(const _NYA_UILayout* layout, f32 placed, f32 weight);

/** How wide text placed next in `layout` may be before it is past its room. */
NYA_INTERNAL f32 _nya_ui_text_room(const _NYA_UILayout* layout);

/**
 * Takes room for a child `natural` wide and high, sized along the container by `own` and the rules above, and across
 * it the whole extent when `fill`.
 * */
NYA_INTERNAL NYA_Rectf _nya_ui_place(NYA_UISize own, f32x2 natural, b8 fill);

/**
 * Registers a focusable widget and works out focus, pointer and activation for it. `horizontal` widgets keep left
 * and right for their value instead of moving between cells.
 * */
NYA_INTERNAL _NYA_UIWidget _nya_ui_widget(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, b8 horizontal);

NYA_INTERNAL void _nya_ui_focus_set(NYA_UI* ui, u64 id);

/** Scrolls the innermost scrolling container so `rect` is inside its view, from the next pass on. */
NYA_INTERNAL void _nya_ui_reveal(NYA_Rectf rect);

/** Moves a draggable top level panel by the pointer on its grip. Updates `state->drag`; the caller clamps it. */
NYA_INTERNAL void _nya_ui_panel_drag(NYA_UI* ui, u64 key, _NYA_UIPanelState* state, NYA_Rectf bounds, f32 header);

/**
 * A typed field for the widget `widget`: edits `buffer` while it has the keyboard, starts on `start`, stops on return,
 * cancel or a press outside `owner`, and draws `box` with the text and caret. True when the text changed.
 * */
NYA_INTERNAL b8 _nya_ui_field(NYA_UI* ui, _NYA_UIWidget widget, b8 start, NYA_Rectf owner, NYA_Rectf box, char* buffer, u32 capacity);

/** Moves `widget`'s focus and press toward its state over the style's transition, and eases them. */
NYA_INTERNAL void _nya_ui_animate(_NYA_UIWidget* widget);

NYA_INTERNAL void _nya_ui_typing_start(NYA_UI* ui, u64 id, u32 caret, NYA_Rectf field);
NYA_INTERNAL void _nya_ui_typing_stop(NYA_UI* ui);

NYA_INTERNAL b8 _nya_ui_drawing(void);

/** Whether `rect`, with room for a pop and a shadow, is drawn this pass: a draw pass, laid out, and inside the clip. */
NYA_INTERNAL b8 _nya_ui_drawn(NYA_Rectf rect);

/** Clips drawing to `clip`, or stops clipping when it is the whole window. */
NYA_INTERNAL void _nya_ui_scissor(const NYA_UI* ui, NYA_Rectf clip);

NYA_INTERNAL f32 _nya_ui_item_height(const _NYA_UILayout* layout);

/** The widget's colour from `colors` for its state. */
NYA_INTERNAL NYA_Color _nya_ui_color(const NYA_UIStateColors* colors, _NYA_UIWidget widget);

/** The widget's skin for its state, a state without a texture taking the normal one's. */
NYA_INTERNAL NYA_UISkin _nya_ui_skin(const NYA_UIStateSkins* skins, _NYA_UIWidget widget);

/** Draws `skin` over `rect`, tinted by `tint`, the flat colour, when the skin has none. False, drawing nothing, for a flat skin or a texture still loading. */
NYA_INTERNAL b8 _nya_ui_skin_draw(const NYA_UI* ui, const NYA_UISkin* skin, NYA_Rectf rect, NYA_Color tint);

/** The shadow, fill, outline and focus mark of a widget, popped when newly focused and sunk when held. Returns the body. */
NYA_INTERNAL NYA_Rectf _nya_ui_body_draw(NYA_UI* ui, NYA_Rectf rect, _NYA_UIWidget widget);

/** A slider or toggle track, a field's box, or the filled part of a track in `color`. */
NYA_INTERNAL void _nya_ui_track_draw(NYA_UI* ui, NYA_Rectf rect, f32 radius, NYA_Color color);

/** `text`, measured `width` wide, centred vertically in `rect` and placed across it by `align`. */
NYA_INTERNAL void _nya_ui_text_draw(NYA_UI* ui, const _NYA_UILayout* layout, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color);

/** `color` with its alpha multiplied by the opacity groups open around it. Identity when none are, so it is free. */
NYA_INTERNAL NYA_Color _nya_ui_fade(NYA_Color color) __attr_no_discard;

/** The bar for `axis` of a scrolling container, drawn in the padding after its content. */
NYA_INTERNAL void _nya_ui_scrollbar_draw(NYA_UI* ui, const _NYA_UILayout* layout, u32 axis);

/** Draws `icon` into `rect`, taking its texture and tint from the style when it names none. False when nothing was drawn. */
NYA_INTERNAL b8 _nya_ui_icon_draw(const NYA_UI* ui, const NYA_UIIcon* icon, NYA_Rectf rect, NYA_Color tint);

/**
 * A row of `count` cells sharing the container, each named by `id` and its index, with the chosen one marked. True
 * when `*selected` changed. Tabs and an open dropdown are the same row with a different mark.
 * */
NYA_INTERNAL b8 _nya_ui_choice_row(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, b8 underline);
