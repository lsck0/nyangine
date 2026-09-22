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
#include "nyangine/ui/ui.h"
#include "nyangine/ui/ui_present.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    /** The top level panel everything here belongs to, U32_MAX outside one. What focus raises when it moves in. */
    u32 root_panel;

    /** The renderer layer drawing lands in while this container is open, so its end can put the layer back. */
    s32 layer;

    /** Placed at a rectangle of its own, taking no room in its container and cut by the window instead. */
    b8 floating;

    /** The padding and outline before the content on each axis, and after it. */
    f32x2 before;
    f32x2 after;

    f32   header;
    f32   title_width;
    f32x2 scroll;

    /** What the pointer can reach and drawing shows. The window, until a scrolling container narrows it. */
    NYA_Rectf clip;

    /** Whether a top level panel above this one holds the pointer, so nothing in here may take it. Inherited inward. */
    b8 covered;

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

    /** Whether the last pass opened it at the top level, which is the only place a panel carries a z order. */
    b8 top_level;

    /** Where it was last laid out, which is what the next pass hit tests the pointer against. */
    NYA_Rectf bounds;

    /** The caller's explicit z. Zero leaves it to `order`. */
    s32 z;

    /**
     * When it was last raised, from _NYA_UISystem.raise_serial. Zero until a top level panel is first declared,
     * which is what makes the stack start in declaration order; a click writes a fresh serial and puts it on top.
     * */
    u64 order;
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

    /** What every widget is drawn and measured through. The shape presenter until a caller says otherwise. */
    const NYA_UIPresenter* present;

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

    /** The window being resized by its corner grip, and where in that corner the pointer took hold. */
    u64   resize_panel;
    f32x2 resize_grip;

    /** The widget that was last activated and when, for the bounce. One at a time, since one pointer clicks one. */
    u64 bounce_id;
    f64 bounce_s;

    /** Where in the dragged panel the pointer grabbed it, so it does not jump to the pointer. */
    f32x2 drag_grip;

    /**
     * The serial of this window's open pass and of the one before it. A panel opened in either is still standing,
     * and one older is a slot the table has not reused yet, which must not occlude anything.
     * */
    u64 pass_current;
    u64 pass_previous;
};

/** What a press is read from: an action, or a raw key when the action is NONE. */
typedef struct {
    NYA_InputAction action;
    NYA_Keycode     key;
} _NYA_UIPress;

/**
 * The menu presses come first and the caret ones after, because a field being typed into keeps the letters and the
 * arrows for itself: everything before _NYA_UI_CARET_LEFT is cleared while the keyboard belongs to a field, which
 * is also what stops tab from leaving a half typed value behind.
 * */
enum {
    _NYA_UI_UP = 0,
    _NYA_UI_DOWN,
    _NYA_UI_LEFT,
    _NYA_UI_RIGHT,
    _NYA_UI_TAB,
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
    u32 widget_panels[NYA_UI_WIDGETS_MAX];
    b8  widget_horizontal[NYA_UI_WIDGETS_MAX];
    u32 widget_count;
    u32 widget_count_worst;
    u32 focus_found;

    /** Rectangles floating containers took this pass. A pointer inside one never reaches what was declared after it. */
    NYA_Rectf claims[NYA_UI_CLAIMS_MAX];
    u32       claim_count;

    /** Whether a panel was grabbed by its grip this pass, so a widget taking the same press can call the grab off. */
    b8 drag_started;

    _NYA_UILayout layouts[NYA_UI_DEPTH_MAX];
    u32           depth;

    /** The window's look and the pushed ones on top of it. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
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

    /** The renderer layer the pass was called in. Top level panels draw in the ones above it, back to front. */
    s32 layer_base;

    _NYA_UIPanelState panels[NYA_UI_PANELS_MAX];
    u32               panel_count;

    /** Counts up forever, so a fresh value is above every order already handed out. */
    u64 raise_serial;

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

/**
 * Builds the look at `depth` through the presenter and selects it, which is what makes every size the layout adds
 * up the backend's rather than this module's. `_at` builds a pushed style instead of the window's own.
 * */
NYA_INTERNAL void              _nya_ui_look_build(const NYA_UI* ui, u32 depth);
NYA_INTERNAL void              _nya_ui_look_build_at(const NYA_UI* ui, u32 depth, const NYA_UIStyle* style);
NYA_INTERNAL const NYA_UILook* _nya_ui_look(void);

/*
 * THE PRESENTER. Everything the module asks of its backend goes through these; see ui_present.h for the shape of it.
 */

/** The open pass's presenter. */
NYA_INTERNAL const NYA_UIPresenter* _nya_ui_present(void) __attr_no_discard;

/** `widget`'s state as a presenter sees it, the two animations resolved to how far through they are. */
NYA_INTERNAL NYA_UIWidgetState _nya_ui_state(const NYA_UI* ui, _NYA_UIWidget widget) __attr_no_discard;

/** Fills in what the pass knows — the text role, the opacity and the clip — and hands `widget` to the presenter. */
NYA_INTERNAL void _nya_ui_draw(NYA_UI* ui, NYA_UIWidgetDraw* widget);

/** What `text` measures at `role`, within `room` and folded by `overflow`, and the width of its first `bytes`. */
NYA_INTERNAL f32x2 _nya_ui_measure(NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) __attr_no_discard;
NYA_INTERNAL f32   _nya_ui_text_width(NYA_UIText role, NYA_ConstCString text) __attr_no_discard;
NYA_INTERNAL f32   _nya_ui_measure_bytes(NYA_UIText role, NYA_ConstCString text, u32 bytes) __attr_no_discard;

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

/** Whether `state` is a top level panel this window opened in its last two passes, and so still stands. */
NYA_INTERNAL b8 _nya_ui_panel_standing(const NYA_UI* ui, const _NYA_UIPanelState* state) __attr_no_discard;

/** Whether `panel` sits over `under`: a higher z, or the same z and raised more recently. */
NYA_INTERNAL b8 _nya_ui_panel_over(const _NYA_UIPanelState* panel, const _NYA_UIPanelState* under) __attr_no_discard;

/** Whether a standing panel over the one in slot `index` holds the pointer, from where each was last laid out. */
NYA_INTERNAL b8 _nya_ui_panel_covered(const NYA_UI* ui, u32 index) __attr_no_discard;

/** Puts the panel in slot `index` over every other standing one of its z. Idempotent when it is already there. */
NYA_INTERNAL void _nya_ui_panel_raise(const NYA_UI* ui, u32 index);

/** How many standing panels the one in slot `index` sits over, which is the layer it draws in above the pass's. */
NYA_INTERNAL u32 _nya_ui_panel_rank(const NYA_UI* ui, u32 index) __attr_no_discard;

/**
 * What nya_ui_panel_begin is. `at` null places the container in its parent as a top level or nested panel; non-null
 * floats it there instead, in window pixels, with a zero width or height resolved from `panel`'s own sizes.
 * */
NYA_INTERNAL b8 _nya_ui_panel_open(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel, const NYA_Rectf* at) __attr_no_discard;

/**
 * A container that hangs over what follows it: placed at `at`, taking no room, cut by the window rather than by the
 * panel that opened it, drawn over every panel, and claiming its rectangle for the rest of the pass as it ends.
 *
 * False when the container table is full; skip the contents and the end. Named, since it takes no place in its
 * container to be remembered by.
 * */
NYA_INTERNAL b8   _nya_ui_float_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel, NYA_Rectf at) __attr_no_discard;
NYA_INTERNAL void _nya_ui_float_end(NYA_UI* ui);

/** Whether a float already took `point` this pass, so nothing declared since may have it. */
NYA_INTERNAL b8 _nya_ui_claimed(f32x2 point) __attr_no_discard;

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

/**
 * Moves a draggable top level panel by the pointer on its grip. Updates `state->drag`; the caller clamps it. A
 * `covered` panel cannot be grabbed, so a grip under another panel is not a handle.
 * */
NYA_INTERNAL void _nya_ui_panel_drag(NYA_UI* ui, u64 key, _NYA_UIPanelState* state, NYA_Rectf bounds, f32 header, b8 covered);

/**
 * A typed field for the widget `widget`: edits `buffer` while it has the keyboard, starts on `start`, and stops on
 * return, cancel or a press outside `owner`. True when the text changed.
 *
 * It draws nothing itself. `out` is filled with what the presenter needs to show the box, the line, the selection
 * and the caret, so the widget that owns the field sends it out with the rest of itself in one call.
 * */
NYA_INTERNAL b8 _nya_ui_field(NYA_UI* ui, _NYA_UIWidget widget, b8 start, NYA_Rectf owner, NYA_Rectf box, char* buffer, u32 capacity, NYA_UIFieldDraw* out);

/** Moves `widget`'s focus and press toward its state over the style's transition, and eases them. */
NYA_INTERNAL void _nya_ui_animate(_NYA_UIWidget* widget);

NYA_INTERNAL void _nya_ui_typing_start(NYA_UI* ui, u64 id, u32 caret, NYA_Rectf field);
NYA_INTERNAL void _nya_ui_typing_stop(NYA_UI* ui);

NYA_INTERNAL b8 _nya_ui_drawing(void);

/** Whether `rect`, with room for a pop and a shadow, is drawn this pass: a draw pass, laid out, and inside the clip. */
NYA_INTERNAL b8 _nya_ui_drawn(NYA_Rectf rect);

/** Clips drawing to `clip`, or stops clipping when it is the whole window. */
NYA_INTERNAL void _nya_ui_scissor(const NYA_UI* ui, NYA_Rectf clip);

/** The renderer layer drawing lands in, and what it is set to. The renderer paints low to high, not in call order. */
NYA_INTERNAL s32  _nya_ui_layer_get(const NYA_UI* ui) __attr_no_discard;
NYA_INTERNAL void _nya_ui_layer_set(const NYA_UI* ui, s32 layer);

NYA_INTERNAL f32 _nya_ui_item_height(const _NYA_UILayout* layout);

/** The bar for `axis` of a scrolling container, placed in the padding after its content and sent to the presenter. */
NYA_INTERNAL void _nya_ui_scrollbar_draw(NYA_UI* ui, const _NYA_UILayout* layout, u32 axis);

/**
 * A row of `count` cells sharing the container, each named by `id` and its index, with the chosen one marked. True
 * when `*selected` changed.
 * */
NYA_INTERNAL b8 _nya_ui_choice_row(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, b8 underline);

/**
 * The same cells as a column floating at `at`, `width` wide, for a dropdown's list and a window's menu. True when
 * `*selected` changed, which is also when the caller closes the list.
 * */
NYA_INTERNAL b8 _nya_ui_choice_list(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, f32x2 at, f32 width);

/** One square chrome button in a window's title bar, named `label` and drawn as `mark`. True when activated. */
NYA_INTERNAL b8 _nya_ui_chrome_button(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, NYA_UIMark mark) __attr_no_discard;

/**
 * Drags a window's bottom right corner by the pointer on `grip`, writing what it leaves to `state->size` in pixels
 * at scale 1. The next pass is what places the window at it, which is this pass's draw pass.
 * */
NYA_INTERNAL void _nya_ui_window_resize(NYA_UI* ui, u64 key, NYA_UIWindowState* state, NYA_Rectf bounds, NYA_Rectf grip);
