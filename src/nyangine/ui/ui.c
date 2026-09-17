/**
 * @file ui.c
 *
 * See ui.h.
 * */
#include "nyangine/nyangine.h"

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

    /** Into _NYA_UISystem.panels, or U32_MAX for the root. */
    u32         panel;
    NYA_UIPanel options;
    NYA_Rectf   bounds;

    /** The padding and outline before the content on each axis, and after it. */
    f32x2 before;
    f32x2 after;

    f32 header;
    f32 title_width;
    f32 scroll;

    /** What the pointer can reach and drawing shows. The window, until a scrolling container narrows it. */
    NYA_Rectf clip;

    /** Whether the content is taller than the container, and whether this pass set a scissor for it. */
    b8 scrolls;
    b8 clipping;

    /** Lays out without drawing: a container with no measurements yet, or a font still loading. Inherited inward. */
    b8 hidden;
} _NYA_UILayout;

typedef struct {
    /** Zero is a free slot. */
    u64 id;

    /** The pass that last opened it, so a full table only refuses containers that pass is still using. */
    u64 pass;

    /** What the last pass measured: the natural outer size, the content's length along the direction, and its children. */
    f32x2 size;
    b8    measured;
    f32   content;
    f32   fixed;
    f32   grow;
    u32   count;

    f32 scroll;

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

/** In the order above. The caret keys are raw keys, since a field has to tell the arrows from A and D. */
NYA_INTERNAL const _NYA_UIPress _NYA_UI_PRESSES[_NYA_UI_PRESS_COUNT] = {
    { .action = NYA_INPUT_ACTION_UP },
    { .action = NYA_INPUT_ACTION_DOWN },
    { .action = NYA_INPUT_ACTION_LEFT },
    { .action = NYA_INPUT_ACTION_RIGHT },
    { .key = NYA_KEY_LEFT },
    { .key = NYA_KEY_RIGHT },
    { .key = NYA_KEY_BACKSPACE },
    { .key = NYA_KEY_DELETE },
};

typedef struct {
    NYA_UI windows[NYA_WINDOW_MAX];

    /** The one pass open, whose scratch is everything below. */
    NYA_UI* open;
    u64     pass_serial;
    b8      registered;

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
    f32   wheel;

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

    _NYA_UIPanelState panels[NYA_UI_PANELS_MAX];
    u32               panel_count;

    _NYA_UIAnimation animations[NYA_UI_ANIMATIONS_MAX];
} _NYA_UISystem;

/* Zeroed, so the tables cost the binary nothing. */
NYA_INTERNAL _NYA_UISystem _nya_ui = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PASSES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_UI* nya_ui_begin(NYA_Window* window, NYA_UIPass pass) {
    nya_assert(window != nullptr);
    nya_assert(pass < NYA_UI_PASS_COUNT);
    nya_assert(_nya_ui.open == nullptr, "a UI pass is already open; end it before beginning another");

    if (!_nya_ui.registered) {
        nya_ceiling_register("ui_widgets", NYA_UI_WIDGETS_MAX, &_nya_ui.widget_count_worst);
        nya_ceiling_register("ui_panels", NYA_UI_PANELS_MAX, &_nya_ui.panel_count);
        _nya_ui.registered = true;
    }

    NYA_UI* ui = _nya_ui_context(window);
    ui->window = window;
    ui->pass   = pass;
    ui->scale  = _nya_ui_scale_derive(window, &ui->style);

    _nya_ui.open            = ui;
    _nya_ui.pass_serial    += 1;
    _nya_ui.widget_count    = 0;
    _nya_ui.focus_found     = U32_MAX;
    _nya_ui.depth           = 0;
    _nya_ui.editing_seen    = false;
    _nya_ui.typing_at_begin = ui->editing != 0;
    _nya_ui.looks[0]        = _nya_ui_look_build(&ui->style, ui->scale);
    _nya_ui.look_depth      = 0;
    _nya_ui.next_set        = false;
    _nya_ui.disabled        = 0;

    NYA_Rectf screen = { 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height };
    NYA_Rectf safe   = screen;

    // no platform window in a headless test, and then the whole target is safe.
    if (window->sdl_window != nullptr) {
        NYA_Rect area    = nya_window_safe_area(window->handle);
        f32      density = nya_window_pixel_density(window->handle);

        if (area.width > 0 && area.height > 0) safe = (NYA_Rectf){ (f32)area.x * density, (f32)area.y * density, (f32)area.width * density, (f32)area.height * density };
    }

    f32 margin   = _nya_ui.looks[0].margin;
    _nya_ui.safe = (NYA_Rectf){ safe.x + margin, safe.y + margin, nya_max(safe.width - (margin * 2.0F), 0.0F), nya_max(safe.height - (margin * 2.0F), 0.0F) };

    _nya_ui_input_read(ui);

    _NYA_UILayout* root = _nya_ui_layout_push();
    *root               = (_NYA_UILayout){
        .extent   = { screen.width, screen.height },
        .room     = { screen.width, screen.height },
        .main     = 1,
        .gap      = _nya_ui.looks[0].spacing,
        .overflow = ui->style.overflow,
        .text     = NYA_UI_TEXT_BODY,
        .key      = (u64)window->handle.index + 1,
        .scope    = (u64)window->handle.index + 1,
        .group    = U32_MAX,
        .panel    = U32_MAX,
        .clip     = screen,
        .hidden   = _nya_ui.looks[0].line_heights[NYA_UI_TEXT_BODY] <= 0.0F,
    };

    return ui;
}

void nya_ui_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth == 1, "%u containers are still open", _nya_ui.depth - 1);
    nya_assert(_nya_ui.look_depth == 0, "%u pushed styles were not popped", _nya_ui.look_depth);
    nya_assert(_nya_ui.disabled == 0, "%u disabled blocks were not ended", _nya_ui.disabled);

    u32 count = _nya_ui.widget_count;

    if (count > 0) {
        // a focused widget that vanished, as every label does when the locale changes, hands focus to its position.
        u32 index  = _nya_ui.focus_found != U32_MAX ? _nya_ui.focus_found : nya_min(ui->focus_index, count - 1);
        u32 before = index;

        const u32* groups = _nya_ui.widget_groups;
        const b8*  press  = _nya_ui.presses;

        // a group is one line: a lone widget, or every focusable cell of a row. up and down keep the position in it.
        u32 first    = groups[index];
        u32 position = index - first;

        if (press[_NYA_UI_UP]) {
            // adding count - 1 wraps upward without an unsigned 0 - 1.
            u32 last = (first + count - 1) % count;
            index    = nya_min(groups[last] + position, last);
        } else if (press[_NYA_UI_DOWN]) {
            u32 next = first;
            while (next < count && groups[next] == first) next++;
            if (next == count) next = 0;

            u32 last = next;
            while (last + 1 < count && groups[last + 1] == next) last++;

            index = nya_min(next + position, last);
        } else if (press[_NYA_UI_LEFT] && !_nya_ui.widget_horizontal[index] && index > first) {
            index -= 1;
        } else if (press[_NYA_UI_RIGHT] && !_nya_ui.widget_horizontal[index] && index + 1 < count && groups[index + 1] == first) {
            index += 1;
        }

        _nya_ui_focus_set(ui, _nya_ui.widgets[index]);
        ui->focus_index = index;

        if (index != before) ui->reveal = true;
    }

    if (ui->pass == NYA_UI_PASS_INPUT) {
        if (!_nya_ui.pointer_down) {
            ui->active   = 0;
            ui->dragging = false;
            ui->grab     = 0;
        }

        // the field left the UI while typing, as when its menu closes.
        if (ui->editing != 0 && !_nya_ui.editing_seen) _nya_ui_typing_stop(ui);

        ui->typing = _nya_ui.typing_at_begin || ui->editing != 0;
    }

    _nya_ui.widget_count_worst = nya_max(_nya_ui.widget_count_worst, count);
    _nya_ui.depth              = 0;
    _nya_ui.open               = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYOUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_panel_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(panel.anchor < NYA_UI_ANCHOR_COUNT && panel.direction < NYA_UI_DIRECTION_COUNT && panel.align < NYA_UI_ALIGN_COUNT);
    nya_assert(panel.overflow < NYA_UI_OVERFLOW_COUNT && panel.text < NYA_UI_TEXT_COUNT);
    nya_assert(panel.width.kind < NYA_UI_SIZE_COUNT && panel.height.kind < NYA_UI_SIZE_COUNT && panel.children.kind < NYA_UI_SIZE_COUNT);
    nya_assert(panel.gap >= 0.0F && panel.padding >= 0.0F);

    const _NYA_UILayout* parent = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();
    const NYA_UISkin*    skin   = &look->style.panel_skin;

    u64 key   = id != nullptr ? _nya_ui_id(parent->scope, id) : _nya_ui_id_at(parent->key, parent->count);
    u32 index = _nya_ui_panel_claim(key);

    if (index == U32_MAX) {
        _nya_ui.next_set = false;
        return false;
    }

    _NYA_UIPanelState* state = &_nya_ui.panels[index];
    state->pass              = _nya_ui.pass_serial;

    NYA_UIText text = panel.text != NYA_UI_TEXT_INHERIT ? panel.text : parent->text;

    // frameless is only what it asks for; a frame pads by its skin's insets or the style's padding, inside the outline.
    f32x2 before = { _nya_ui_px(panel.padding), _nya_ui_px(panel.padding) };
    f32x2 after  = before;

    if (!panel.frameless && panel.padding <= 0.0F && skin->texture[0] != '\0') {
        before = (f32x2){ _nya_ui_px(skin->left), _nya_ui_px(skin->top) };
        after  = (f32x2){ _nya_ui_px(skin->right), _nya_ui_px(skin->bottom) };
    } else if (!panel.frameless) {
        f32 pad = (panel.padding > 0.0F ? before.x : look->padding) + look->outline;
        before  = (f32x2){ pad, pad };
        after   = before;
    }

    f32 header      = 0.0F;
    f32 title_width = 0.0F;

    if (panel.title != nullptr) {
        header      = look->line_heights[NYA_UI_TEXT_TITLE] + look->spacing;
        title_width = nya_font_width(look->fonts[NYA_UI_TEXT_TITLE], panel.title);
    }

    f32x2            chrome   = { before.x + after.x, before.y + after.y + header };
    const NYA_UISize sizes[2] = { panel.width, panel.height };

    NYA_Rectf bounds;
    f32x2     room;

    if (_nya_ui.depth == 1) {
        const NYA_Rectf* safe = &_nya_ui.safe;

        f32x2 size = {
            _nya_ui_extent(panel.width, state->size.x, safe->width),
            _nya_ui_extent(panel.height, state->size.y, safe->height),
        };

        // 0, 0.5 or 1 across and down, so one expression anchors to either edge or the middle.
        u32   column = panel.anchor % 3;
        u32   row    = panel.anchor / 3;
        f32   across = (f32)column * 0.5F;
        f32   down   = (f32)row * 0.5F;
        f32x2 offset = { _nya_ui_px(panel.offset.x), _nya_ui_px(panel.offset.y) };

        bounds = (NYA_Rectf){
            .x      = roundf(safe->x + ((safe->width - size.x) * across) + (offset.x * (1.0F - (across * 2.0F)))),
            .y      = roundf(safe->y + ((safe->height - size.y) * down) + (offset.y * (1.0F - (down * 2.0F)))),
            .width  = size.x,
            .height = size.y,
        };

        // slides up into place when it shows after being gone, on the wall clock so a paused world still animates.
        if (look->style.appear_s > 0.0F) {
            f64 now = nya_app_uptime_s();
            if (now - state->seen_s > NYA_UI_APPEAR_GAP_S) state->shown_s = now;
            state->seen_s = now;

            f32 t     = nya_ease(look->style.easing, nya_clamp((f32)(now - state->shown_s) / look->style.appear_s, 0.0F, 1.0F));
            bounds.y += roundf((1.0F - t) * _nya_ui_px(NYA_UI_APPEAR_OFFSET));
        }

        // what the safe area leaves past the offset, as a margin on both sides unless the anchor centres that axis.
        room = (f32x2){ safe->width - (across == 0.5F ? 0.0F : offset.x * 2.0F), safe->height - (down == 0.5F ? 0.0F : offset.y * 2.0F) };

        for (u32 axis = 0; axis < 2; axis++) {
            if (sizes[axis].kind == NYA_UI_SIZE_FIXED || sizes[axis].kind == NYA_UI_SIZE_GROW) room[axis] = nya_min(room[axis], size[axis]);
            if (sizes[axis].max > 0.0F) room[axis] = nya_min(room[axis], _nya_ui_px(sizes[axis].max));
        }
    } else {
        u32 main  = parent->main;
        u32 cross = 1 - main;

        f32x2 natural = state->size;
        b8    fill    = sizes[cross].kind == NYA_UI_SIZE_AUTO;

        // across, a fixed or growing size is decided here; along, the container decides with the rest of its children.
        f32 parent_across = parent->extent[cross] > 0.0F ? parent->extent[cross] : parent->room[cross];
        if (sizes[cross].kind == NYA_UI_SIZE_FIXED || sizes[cross].kind == NYA_UI_SIZE_GROW) natural[cross] = _nya_ui_extent(sizes[cross], natural[cross], parent_across);

        NYA_UISize along = _nya_ui_next_size(parent, sizes[main]);
        bounds           = _nya_ui_place(sizes[main], natural, fill);

        f32x2 placed = { bounds.width, bounds.height };

        room[cross] = sizes[cross].kind == NYA_UI_SIZE_FIT || placed[cross] <= 0.0F ? parent_across : placed[cross];
        room[main]  = along.kind == NYA_UI_SIZE_FIXED || along.kind == NYA_UI_SIZE_GROW ? placed[main] : parent->room[main];

        for (u32 axis = 0; axis < 2; axis++) {
            if (sizes[axis].max > 0.0F) room[axis] = nya_min(room[axis], _nya_ui_px(sizes[axis].max));
        }
    }

    f32x2 extent = { nya_max(bounds.width - chrome.x, 0.0F), nya_max(bounds.height - chrome.y, 0.0F) };
    u32   main   = panel.direction == NYA_UI_DIRECTION_ROW ? 0 : 1;
    f32   gap    = panel.gap > 0.0F ? _nya_ui_px(panel.gap) : look->spacing;

    // only a column scrolls, when what it measured last pass is taller than it is now.
    b8  scrolls = main == 1 && state->measured && extent.y > 0.0F && state->content > extent.y + 0.5F;
    f32 scroll  = scrolls ? state->scroll : 0.0F;
    f32 top     = bounds.y + before.y + header;

    NYA_Rectf clip = parent->clip;

    if (scrolls) {
        // inside the outline, and a little above the content so a focused widget's edge is not cut.
        f32       inset     = panel.frameless ? 0.0F : look->outline;
        f32       above     = panel.frameless ? top : top - roundf(before.y * 0.5F);
        NYA_Rectf view_clip = { bounds.x + inset, above, bounds.width - (inset * 2.0F), bounds.y + bounds.height - inset - above };

        clip = nya_rect_intersection(parent->clip, view_clip);
    }

    f32 gaps = state->count > 1 ? gap * (f32)(state->count - 1) : 0.0F;

    _NYA_UILayout* layout = _nya_ui_layout_push();
    *layout               = (_NYA_UILayout){
        .origin      = { bounds.x + before.x, top - scroll },
        .extent      = extent,
        .room        = { nya_max(room.x - chrome.x, 0.0F), nya_max(room.y - chrome.y, 0.0F) },
        .main        = main,
        .gap         = gap,
        .children    = panel.children,
        .align       = panel.align,
        .overflow    = panel.overflow != NYA_UI_OVERFLOW_INHERIT ? panel.overflow : parent->overflow,
        .text        = text,
        .grow_space  = extent[main] > 0.0F ? nya_max(extent[main] - state->fixed - gaps, 0.0F) : 0.0F,
        .grow_total  = state->grow,
        .key         = key,
        .scope       = id != nullptr ? key : parent->scope,
        .group       = main == 1 ? U32_MAX : (parent->main == 0 ? parent->group : _nya_ui.widget_count),
        .panel       = index,
        .options     = panel,
        .bounds      = bounds,
        .before      = before,
        .after       = after,
        .header      = header,
        .title_width = title_width,
        .scroll      = scroll,
        .clip        = clip,
        .scrolls     = scrolls,
        .hidden      = parent->hidden || !state->measured || look->line_heights[text] <= 0.0F,
    };

    if (!_nya_ui_drawing()) return true;

    NYA_Window*        window = ui->window;
    const NYA_UIStyle* style  = &look->style;

    if (!panel.frameless) {
        NYA_Color fill    = panel.fill;
        b8        colored = fill.r != 0.0F || fill.g != 0.0F || fill.b != 0.0F || fill.a != 0.0F;

        if (!colored) fill = style->panel;

        if (!_nya_ui_skin_draw(ui, skin, bounds, fill)) {
            if (look->depth > 0.0F) nya_render2d_rect_rounded(window, bounds.x, bounds.y + look->depth, bounds.width, bounds.height, look->radius, style->ink);
            nya_render2d_rect_rounded(window, bounds.x, bounds.y, bounds.width, bounds.height, look->radius, fill);
            if (look->outline > 0.0F) nya_render2d_rect_rounded_outline(window, bounds.x, bounds.y, bounds.width, bounds.height, look->radius, look->outline, style->ink);
        }
    }

    if (panel.title != nullptr) {
        NYA_Font title = look->fonts[NYA_UI_TEXT_TITLE];
        f32      x     = roundf(bounds.x + ((bounds.width - title_width) * 0.5F));
        f32      y     = bounds.y + before.y;

        if (look->depth > 0.0F) nya_font_draw(window, title, panel.title, x, y + roundf(look->depth * 0.5F), style->ink);
        nya_font_draw(window, title, panel.title, x, y, style->text.normal);
    }

    if (scrolls) {
        _nya_ui_scissor(ui, clip);
        layout->clipping = true;
    }

    return true;
}

void nya_ui_panel_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1, "no container is open");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    _nya_ui.depth              -= 1;

    const _NYA_UILayout* parent  = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIPanel*   options = &layout->options;
    const _NYA_UILook*   look    = _nya_ui_look();

    f32x2 content = layout->main == 1 ? (f32x2){ layout->across, layout->used } : (f32x2){ layout->used, layout->across };
    f32x2 natural = {
        nya_max(content.x, layout->title_width) + layout->before.x + layout->after.x,
        content.y + layout->header + layout->before.y + layout->after.y,
    };

    const NYA_UISize sizes[2] = { options->width, options->height };

    for (u32 axis = 0; axis < 2; axis++) {
        natural[axis] = nya_max(natural[axis], _nya_ui_px(sizes[axis].min));
        if (sizes[axis].max > 0.0F) natural[axis] = nya_min(natural[axis], _nya_ui_px(sizes[axis].max));
    }

    // a face still loading measures zero, and that size must not stick.
    _NYA_UIPanelState* state = &_nya_ui.panels[layout->panel];
    state->size              = (f32x2){ ceilf(natural.x), ceilf(natural.y) };
    state->measured          = look->line_heights[layout->text] > 0.0F;
    state->content           = layout->used;
    state->fixed             = layout->fixed;
    state->grow              = layout->grow;
    state->count             = layout->count;

    if (layout->main == 1) {
        f32 reach = layout->extent.y > 0.0F ? nya_max(layout->used - layout->extent.y, 0.0F) : 0.0F;

        // the innermost scrolling container under the pointer takes the wheel, since it ends first.
        if (reach > 0.0F && _nya_ui.wheel != 0.0F && nya_rect_contains(nya_rect_intersection(layout->bounds, parent->clip), _nya_ui.pointer)) {
            state->scroll -= _nya_ui.wheel * _nya_ui_px(NYA_UI_SCROLL_STEP);
            _nya_ui.wheel  = 0.0F;
        }

        state->scroll = nya_clamp(state->scroll, 0.0F, reach);
    }

    if (!layout->clipping) return;

    _nya_ui_scissor(ui, parent->clip);

    // the bar shows the scroll this pass drew with, in the padding after the content.
    const NYA_Rectf* bounds = &layout->bounds;

    f32 bar   = _nya_ui_px(NYA_UI_SCROLLBAR);
    f32 track = layout->extent.y;
    f32 total = nya_max(layout->used, track);
    f32 thumb = nya_max(roundf(track * (track / total)), bar * 4.0F);
    f32 t     = total > track ? nya_clamp(layout->scroll / (total - track), 0.0F, 1.0F) : 0.0F;
    f32 x     = roundf(bounds->x + bounds->width - ((layout->after.x + bar) * 0.5F));
    f32 y     = roundf(layout->origin.y + layout->scroll + ((track - thumb) * t));

    nya_render2d_rect_rounded(ui->window, x, y, bar, thumb, bar * 0.5F, look->style.text_dim);
}

void nya_ui_size(NYA_UI* ui, NYA_UISize size) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(size.kind < NYA_UI_SIZE_COUNT && size.value >= 0.0F && size.min >= 0.0F && size.max >= 0.0F);

    _nya_ui.next     = size;
    _nya_ui.next_set = true;
}

NYA_Rectf nya_ui_space(NYA_UI* ui, f32 width, f32 height) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(width >= 0.0F && height >= 0.0F);

    const _NYA_UILayout* layout  = &_nya_ui.layouts[_nya_ui.depth - 1];
    f32x2                natural = { _nya_ui_px(width), _nya_ui_px(height) };

    return _nya_ui_place((NYA_UISize){ 0 }, natural, natural[1 - layout->main] == 0.0F);
}

void nya_ui_scrim(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    if (ui->pass != NYA_UI_PASS_DRAW) return;

    nya_render2d_rect(ui->window, 0.0F, 0.0F, (f32)ui->window->screen_width, (f32)ui->window->screen_height, _nya_ui_look()->style.scrim);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text) __attr_overloaded {
    nya_assert(ui != nullptr);

    nya_ui_label(ui, text, _nya_ui_look()->style.text.normal);
}

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text, NYA_Color color) __attr_overloaded {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(text != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    if (_nya_ui.disabled > 0) color = look->style.text.disabled;

    NYA_Font font  = look->fonts[layout->text];
    f32      line  = look->line_heights[layout->text];
    f32      room  = _nya_ui_text_room(layout);
    f32      width = nya_font_width(font, text);
    b8       over  = room > 0.0F && width > room;

    if (over && layout->overflow == NYA_UI_OVERFLOW_WRAP) {
        f32x2     size = nya_font_measure_wrapped(font, text, room);
        NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(size.x), ceilf(size.y) }, false);

        // NYA_UIAlign and NYA_TextAlign share their order.
        if (_nya_ui_drawn(rect)) nya_font_draw_wrapped(ui->window, font, text, rect.x, rect.y, rect.width, (NYA_TextAlign)layout->align, color);
        return;
    }

    if (over && layout->overflow == NYA_UI_OVERFLOW_SHRINK) {
        // whole points, so a line that changes a digit a frame does not bake an atlas a frame.
        font.point_size = nya_max(floorf(font.point_size * (room / width)), 1.0F);
        width           = nya_font_width(font, text);

        if (width > room && font.point_size > 1.0F) {
            font.point_size -= 1.0F;
            width            = nya_font_width(font, text);
        }

        // a size not loaded yet measures zero; holding the room keeps the container from collapsing for that frame.
        if (width <= 0.0F) width = room;
    }

    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(width), line }, false);
    if (!_nya_ui_drawn(rect)) return;

    // a shrunk line keeps the line height of its size, centred in it.
    f32 y = font.point_size != look->fonts[layout->text].point_size ? roundf(rect.y + ((line - nya_font_metrics(font).line_height) * 0.5F)) : rect.y;

    nya_font_draw(ui->window, font, text, rect.x, y, color);
}

b8 nya_ui_button(NYA_UI* ui, NYA_ConstCString label) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 2.0F), _nya_ui_item_height(layout) }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget);
        _nya_ui_text_draw(ui, layout, label, text_width, body, NYA_UI_ALIGN_CENTER, _nya_ui_color(&look->style.text, widget));
    }

    return widget.activated;
}

b8 nya_ui_selectable(NYA_UI* ui, NYA_ConstCString label, b8 selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 dot    = roundf(height * 0.14F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (dot * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf body   = _nya_ui_body_draw(ui, rect, widget);
        f32x2     center = { body.x + look->padding + dot, body.y + roundf(body.height * 0.5F) };

        // an empty hole when not chosen, so the list reads as a choice even before anything is picked.
        nya_render2d_circle(ui->window, center, dot, selected && !widget.disabled ? look->style.accent : look->style.track);

        NYA_Rectf text = { body.x + (dot * 2.0F) + look->padding, body.y, body.width - (dot * 2.0F) - look->padding, body.height };
        _nya_ui_text_draw(ui, layout, label, text_width, text, NYA_UI_ALIGN_CENTER, _nya_ui_color(&look->style.text, widget));
    }

    return widget.activated;
}

b8 nya_ui_toggle(NYA_UI* ui, NYA_ConstCString label, b8* value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height      = _nya_ui_item_height(layout);
    f32 pill_height = roundf(height * 0.5F);
    f32 pill_width  = roundf(pill_height * 1.8F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + pill_width, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    b8 before = *value;

    if (widget.activated) *value = !*value;
    if (widget.focused && _nya_ui.presses[_NYA_UI_LEFT]) *value = false;
    if (widget.focused && _nya_ui.presses[_NYA_UI_RIGHT]) *value = true;

    if (_nya_ui_drawn(rect)) {
        const NYA_UIStyle* style = &look->style;
        NYA_Rectf          body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color = _nya_ui_color(&style->text, widget);

        NYA_Rectf pill = { body.x + body.width - look->padding - pill_width, roundf(body.y + ((body.height - pill_height) * 0.5F)), pill_width, pill_height };
        _nya_ui_track_draw(ui, pill, pill_height * 0.5F, *value && !widget.disabled ? style->accent : style->track);

        f32   radius = (pill_height * 0.5F) - nya_max(look->outline, _nya_ui_px(2.0F));
        f32x2 center = { *value ? pill.x + pill_width - (pill_height * 0.5F) : pill.x + (pill_height * 0.5F), pill.y + (pill_height * 0.5F) };
        NYA_Rectf knob = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

        if (!_nya_ui_skin_draw(ui, &style->knob_skin, knob, color)) nya_render2d_circle(ui->window, center, radius, color);

        // text after every shape, so the shapes of the whole menu can share a draw call.
        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
    }

    return *value != before;
}

b8 nya_ui_slider(NYA_UI* ui, NYA_ConstCString label, f32* value, f32 min, f32 max, f32 step) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);
    nya_assert(max > min && step >= 0.0F, "a slider needs min below max and a step of zero or more");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 radius = roundf(height * 0.2F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (height * 2.0F), height }, true);

    // the right part of the row, past the label, inset by the knob so it can reach both ends.
    f32 track_x     = roundf(rect.x + nya_max(rect.width * 0.5F, text_width + (look->padding * 2.0F)) + radius);
    f32 track_width = nya_max((rect.x + rect.width - look->padding - radius) - track_x, 1.0F);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    f32 before = *value;

    if (ui->pass == NYA_UI_PASS_INPUT && !widget.disabled) {
        if (widget.focused && _nya_ui.presses[_NYA_UI_LEFT]) *value -= step;
        if (widget.focused && _nya_ui.presses[_NYA_UI_RIGHT]) *value += step;

        // a press past the label grabs the knob; one on the label only focuses the row.
        if (_nya_ui.pointer_pressed && ui->active == widget.id && _nya_ui.pointer.x >= track_x - (radius * 2.0F)) ui->dragging = true;

        if (ui->active == widget.id && ui->dragging && _nya_ui.pointer_down) {
            f32 t  = nya_clamp((_nya_ui.pointer.x - track_x) / track_width, 0.0F, 1.0F);
            *value = min + (t * (max - min));

            if (step > 0.0F) *value = min + (roundf((*value - min) / step) * step);
        }

        *value = nya_clamp(*value, min, max);
    }

    if (_nya_ui_drawn(rect)) {
        const NYA_UIStyle* style = &look->style;
        NYA_Rectf          body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color = _nya_ui_color(&style->text, widget);

        f32       t     = nya_clamp((*value - min) / (max - min), 0.0F, 1.0F);
        NYA_Rectf track = { track_x + (body.x - rect.x), roundf(body.y + ((body.height - radius) * 0.5F)), track_width, radius };

        _nya_ui_track_draw(ui, track, radius * 0.5F, style->track);
        _nya_ui_track_draw(ui, (NYA_Rectf){ track.x, track.y, roundf(track_width * t), radius }, radius * 0.5F, widget.disabled ? style->track : style->accent);

        f32x2     center = { track.x + roundf(track_width * t), track.y + (radius * 0.5F) };
        NYA_Rectf knob   = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

        if (!_nya_ui_skin_draw(ui, &style->knob_skin, knob, color)) {
            if (look->outline > 0.0F) nya_render2d_circle(ui->window, center, radius + look->outline, style->ink);
            nya_render2d_circle(ui->window, center, radius, color);
        }

        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
    }

    return *value != before;
}

b8 nya_ui_text_input(NYA_UI* ui, NYA_ConstCString label, char* buffer, u32 capacity) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && buffer != nullptr);
    nya_assert(capacity > 0 && capacity <= NYA_UI_TEXT_INPUT_MAX, "a field edits 1 to NYA_UI_TEXT_INPUT_MAX bytes, got %u", capacity);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 line   = look->line_heights[layout->text];
    f32 height = _nya_ui_item_height(layout);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (height * 3.0F), height }, true);

    // the right part of the row past the label, like a slider's track.
    f32       field_x = roundf(rect.x + nya_max(rect.width * 0.4F, text_width + (look->padding * 2.0F)));
    f32       inset   = roundf(nya_min(look->padding * 0.5F, (height - line) * 0.5F));
    NYA_Rectf box     = { field_x, rect.y + inset, nya_max(rect.x + rect.width - look->padding - field_x, 1.0F), height - (inset * 2.0F) };

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    // the box follows the body when it sinks.
    NYA_Rectf body    = _nya_ui_drawn(rect) ? _nya_ui_body_draw(ui, rect, widget) : rect;
    NYA_Rectf sunk    = nya_rect_translate(box, (f32x2){ body.x - rect.x, body.y - rect.y });
    b8        changed = _nya_ui_field(ui, widget, widget.activated, rect, sunk, buffer, capacity);

    if (_nya_ui_drawn(rect)) {
        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START,
                          _nya_ui_color(&look->style.text, widget));
    }

    return changed;
}

b8 nya_ui_color_picker(NYA_UI* ui, NYA_ConstCString label, NYA_Color* color) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && color != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();
    const NYA_UIStyle*   style  = &look->style;

    f32 height  = _nya_ui_item_height(layout);
    f32 gap     = look->spacing;
    f32 padding = look->padding;
    f32 field   = _nya_ui_px(NYA_UI_PICKER_FIELD);
    f32 bar     = _nya_ui_px(NYA_UI_PICKER_BAR);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (padding * 3.0F) + (height * 4.0F), height + field + bar + (gap * 2.0F) + padding }, true);

    // the top row is the label, then a swatch and the hex field; under it the field and hue bar, then the alpha bar.
    f32       inset  = roundf(padding * 0.5F);
    f32       row_x  = roundf(rect.x + nya_max(rect.width * 0.4F, text_width + (padding * 2.0F)));
    NYA_Rectf swatch = { row_x, rect.y + inset, height - (inset * 2.0F), height - (inset * 2.0F) };
    NYA_Rectf hex    = { swatch.x + swatch.width + gap, swatch.y, nya_max(rect.x + rect.width - padding - swatch.x - swatch.width - gap, 1.0F), swatch.height };
    NYA_Rectf plane  = { rect.x + padding, rect.y + height + gap, nya_max(rect.width - (padding * 2.0F) - gap - bar, 1.0F), field };
    NYA_Rectf hue    = { plane.x + plane.width + gap, plane.y, bar, field };
    NYA_Rectf alpha  = { plane.x, plane.y + field + gap, rect.width - (padding * 2.0F), bar };

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    NYA_ColorHSV hsv    = nya_color_to_hsv(*color);
    NYA_Color    before = *color;

    // a grey has no hue of its own, so the one it was dragged from is kept while this picker holds focus.
    if (ui->hue_id == widget.id && (hsv.s <= 0.0F || hsv.v <= 0.0F)) hsv.h = ui->hue;

    if (ui->pass == NYA_UI_PASS_INPUT && !widget.disabled) {
        b8    moved = false;
        f32x2 at    = _nya_ui.pointer;

        if (widget.focused && (_nya_ui.presses[_NYA_UI_LEFT] || _nya_ui.presses[_NYA_UI_RIGHT])) {
            hsv.h = fmodf(hsv.h + (_nya_ui.presses[_NYA_UI_RIGHT] ? 10.0F : 350.0F), 360.0F);
            moved = true;
        }

        // the part a press lands on keeps the pointer until release, however far it is dragged.
        if (_nya_ui.pointer_pressed && ui->active == widget.id) {
            if (nya_rect_contains(plane, at)) ui->grab = 1;
            if (nya_rect_contains(hue, at)) ui->grab = 2;
            if (nya_rect_contains(alpha, at)) ui->grab = 3;
        }

        if (ui->active == widget.id && _nya_ui.pointer_down && ui->grab != 0) {
            if (ui->grab == 1) hsv.s = nya_clamp((at.x - plane.x) / plane.width, 0.0F, 1.0F);
            if (ui->grab == 1) hsv.v = 1.0F - nya_clamp((at.y - plane.y) / plane.height, 0.0F, 1.0F);
            if (ui->grab == 2) hsv.h = nya_clamp((at.y - hue.y) / hue.height, 0.0F, 0.999F) * 360.0F;
            if (ui->grab == 3) hsv.a = nya_clamp((at.x - alpha.x) / alpha.width, 0.0F, 1.0F);
            moved = true;
        }

        // converted only when something moved, so an untouched colour does not drift through the round trip.
        if (moved) *color = nya_color_from_hsv(hsv);
    }

    if (widget.focused) {
        ui->hue_id = widget.id;
        ui->hue    = hsv.h;
    }

    // typed into a copy that outlives the pass, since the colour only changes once the digits are whole.
    char shown[sizeof(ui->hex)];
    u32  packed = nya_color_to_u32(*color);

    if ((packed & 0xFFU) == 0xFFU) (void)snprintf(shown, sizeof(shown), "#%06X", packed >> 8);
    if ((packed & 0xFFU) != 0xFFU) (void)snprintf(shown, sizeof(shown), "#%08X", packed);

    b8 start = ui->pass == NYA_UI_PASS_INPUT && ui->editing != widget.id &&
               ((widget.focused && _nya_ui.confirm) || (ui->active == widget.id && _nya_ui.pointer_released && nya_rect_contains(hex, _nya_ui.pointer)));

    if (start) nya_memcpy(ui->hex, shown, sizeof(shown));

    char*     text = ui->editing == widget.id || start ? ui->hex : shown;
    NYA_Rectf body = _nya_ui_drawn(rect) ? _nya_ui_body_draw(ui, rect, widget) : rect;
    f32x2     sink = { body.x - rect.x, body.y - rect.y };

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf p = nya_rect_translate(plane, sink);
        NYA_Rectf h = nya_rect_translate(hue, sink);
        NYA_Rectf a = nya_rect_translate(alpha, sink);
        NYA_Rectf w = nya_rect_translate(swatch, sink);

        NYA_Color pure  = nya_color_from_hsv((NYA_ColorHSV){ hsv.h, 1.0F, 1.0F, 1.0F });
        NYA_Color white = { 1.0F, 1.0F, 1.0F, 1.0F };
        NYA_Color black = { 0.0F, 0.0F, 0.0F, 1.0F };
        NYA_Color clear = { 0.0F, 0.0F, 0.0F, 0.0F };
        NYA_Color solid = { color->r, color->g, color->b, 1.0F };
        NYA_Color faded = { color->r, color->g, color->b, 0.0F };

        nya_render2d_rect_gradient(ui->window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ white, pure, pure, white });
        nya_render2d_rect_gradient(ui->window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ clear, clear, black, black });

        // six bands of hue, red at both ends.
        for (u32 i = 0; i < 6; i++) {
            NYA_Color top    = nya_color_from_hsv((NYA_ColorHSV){ (f32)i * 60.0F, 1.0F, 1.0F, 1.0F });
            NYA_Color bottom = nya_color_from_hsv((NYA_ColorHSV){ (f32)((i + 1) % 6) * 60.0F, 1.0F, 1.0F, 1.0F });
            f32       y0     = roundf(h.y + (h.height * (f32)i / 6.0F));
            f32       y1     = roundf(h.y + (h.height * (f32)(i + 1) / 6.0F));

            nya_render2d_rect_gradient(ui->window, h.x, y0, h.width, y1 - y0, (NYA_Color[4]){ top, top, bottom, bottom });
        }

        _nya_ui_track_draw(ui, a, 0.0F, style->track);
        nya_render2d_rect_gradient(ui->window, a.x, a.y, a.width, a.height, (NYA_Color[4]){ faded, solid, solid, faded });

        _nya_ui_track_draw(ui, w, look->radius, style->track);
        nya_render2d_rect(ui->window, w.x, w.y, w.width, w.height, *color);

        // where each part stands: a ring on the field, and a notch across each bar.
        f32   mark   = _nya_ui_px(2.0F);
        f32x2 center = { roundf(p.x + (p.width * hsv.s)), roundf(p.y + (p.height * (1.0F - hsv.v))) };

        nya_render2d_circle(ui->window, center, mark * 3.0F, style->text.normal);
        nya_render2d_circle(ui->window, center, mark * 2.0F, solid);
        nya_render2d_rect(ui->window, h.x - mark, roundf(h.y + (h.height * hsv.h / 360.0F)) - mark, h.width + (mark * 2.0F), mark * 2.0F, style->text.normal);
        nya_render2d_rect(ui->window, roundf(a.x + (a.width * hsv.a)) - mark, a.y - mark, mark * 2.0F, a.height + (mark * 2.0F), style->text.normal);
    }

    b8 typed = _nya_ui_field(ui, widget, start, hex, nya_rect_translate(hex, sink), text, sizeof(ui->hex));

    // six or eight digits make a colour; anything shorter waits for the rest.
    NYA_ConstCString digits = text[0] == '#' ? text + 1 : text;
    u64              count  = strlen(digits);
    b8               valid  = count == 6 || count == 8;

    for (u64 i = 0; valid && i < count; i++) valid = isxdigit((unsigned char)digits[i]) != 0;

    if (typed && valid) *color = nya_color_from_hex(digits);

    if (_nya_ui_drawn(rect)) {
        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + padding, body.y, body.width, height }, NYA_UI_ALIGN_START, _nya_ui_color(&style->text, widget));
    }

    return color->r != before.r || color->g != before.g || color->b != before.b || color->a != before.a;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_disabled_begin(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    _nya_ui.disabled += 1;
}

void nya_ui_disabled_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.disabled > 0, "nya_ui_disabled_end without a begin");

    _nya_ui.disabled -= 1;
}

b8 nya_ui_cancelled(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    return _nya_ui.cancel;
}

b8 nya_ui_typing(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->typing;
}

void nya_ui_focus_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_UI* ui = _nya_ui_context(window);
    if (ui->editing != 0) _nya_ui_typing_stop(ui);

    ui->focus       = 0;
    ui->focus_index = 0;
    ui->active      = 0;
    ui->dragging    = false;
    ui->typing      = false;
    ui->reveal      = true;
}

b8 nya_ui_modal_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    switch (event->type) {
        case NYA_EVENT_KEY_DOWN:
        case NYA_EVENT_KEY_UP:
        case NYA_EVENT_TEXT_INPUT:
        case NYA_EVENT_TEXT_EDITING:
        case NYA_EVENT_MOUSE_BUTTON_DOWN:
        case NYA_EVENT_MOUSE_BUTTON_UP:
        case NYA_EVENT_MOUSE_WHEEL_MOVED: {
            event->was_handled = true;
            return true;
        }

        default: return false;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STYLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_style_set(NYA_Window* window, NYA_UIStyle style) {
    nya_assert(window != nullptr);

    _nya_ui_context(window)->style = _nya_ui_style_resolve(style);
}

NYA_UIStyle nya_ui_style_get(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->style;
}

void nya_ui_style_push(NYA_UI* ui, NYA_UIStyle style) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.look_depth < NYA_UI_STYLE_DEPTH_MAX, "styles pushed deeper than NYA_UI_STYLE_DEPTH_MAX");

    NYA_UIStyle resolved = _nya_ui_style_resolve(style);

    _nya_ui.look_depth                += 1;
    _nya_ui.looks[_nya_ui.look_depth]  = _nya_ui_look_build(&resolved, ui->scale);
}

void nya_ui_style_pop(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.look_depth > 0, "nya_ui_style_pop without a push");

    _nya_ui.look_depth -= 1;
}

f32 nya_ui_scale(const NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_UI* ui = _nya_ui_context(window);

    return ui->scale > 0.0F ? ui->scale : 1.0F;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_UI* _nya_ui_context(const NYA_Window* window) {
    nya_assert(window->handle.index < NYA_WINDOW_MAX);

    NYA_UI* ui = &_nya_ui.windows[window->handle.index];

    b8 same = ui->claimed && ui->handle.generation == window->handle.generation;
    if (!same) *ui = (NYA_UI){ .claimed = true, .handle = window->handle, .style = _nya_ui_style_resolve((NYA_UIStyle){ 0 }) };

    return ui;
}

NYA_UIStyle _nya_ui_style_resolve(NYA_UIStyle style) {
    style.font[NYA_UI_FONT_NAME_MAX - 1]       = '\0';
    style.title_font[NYA_UI_FONT_NAME_MAX - 1] = '\0';

    struct {
        f32* value;
        f32  fallback;
    } sizes[] = {
        { &style.body_size,        NYA_UI_BODY_SIZE        },
        { &style.small_size,       NYA_UI_SMALL_SIZE       },
        { &style.title_size,       NYA_UI_TITLE_SIZE       },
        { &style.reference_height, NYA_UI_REFERENCE_HEIGHT },
        { &style.margin,           NYA_UI_MARGIN           },
        { &style.padding,          NYA_UI_PADDING          },
        { &style.spacing,          NYA_UI_SPACING          },
        { &style.radius,           NYA_UI_RADIUS           },
        { &style.outline,          NYA_UI_OUTLINE          },
        { &style.depth,            NYA_UI_DEPTH            },
        { &style.pop,              NYA_UI_POP              },
    };

    for (u32 i = 0; i < nya_carray_length(sizes); i++) {
        if (*sizes[i].value <= 0.0F) *sizes[i].value = sizes[i].fallback;
    }

    if (style.scale < 0.0F) style.scale = 0.0F;
    if (style.item_height < 0.0F) style.item_height = 0.0F;

    // from a hand edited config, so a bad value falls back instead of asserting.
    if (style.overflow == NYA_UI_OVERFLOW_INHERIT || style.overflow >= NYA_UI_OVERFLOW_COUNT) style.overflow = NYA_UI_OVERFLOW_VISIBLE;

    NYA_UISkin* skins[] = { &style.panel_skin, &style.button_skin.normal, &style.button_skin.focused, &style.button_skin.pressed,
                            &style.button_skin.disabled, &style.track_skin, &style.knob_skin };

    for (u32 i = 0; i < nya_carray_length(skins); i++) skins[i]->texture[NYA_UI_SKIN_TEXTURE_MAX - 1] = '\0';

    struct {
        NYA_Color* color;
        NYA_Color  fallback;
    } colors[] = {
        { &style.scrim,           NYA_UI_SCRIM           },
        { &style.panel,           NYA_UI_PANEL           },
        { &style.ink,             NYA_UI_INK             },
        { &style.track,           NYA_UI_TRACK           },
        { &style.accent,          NYA_UI_ACCENT          },
        { &style.text_dim,        NYA_UI_TEXT_DIM        },
        { &style.button.normal,   NYA_UI_BUTTON          },
        { &style.button.focused,  NYA_UI_BUTTON_FOCUSED  },
        { &style.button.pressed,  NYA_UI_BUTTON_PRESSED  },
        { &style.button.disabled, NYA_UI_BUTTON_DISABLED },
        { &style.text.normal,     NYA_UI_TEXT            },
        { &style.text.focused,    NYA_UI_TEXT_FOCUSED    },
        { &style.text.pressed,    NYA_UI_TEXT_PRESSED    },
        { &style.text.disabled,   NYA_UI_TEXT_DISABLED   },
    };

    for (u32 i = 0; i < nya_carray_length(colors); i++) {
        NYA_Color c = *colors[i].color;
        if (c.r == 0.0F && c.g == 0.0F && c.b == 0.0F && c.a == 0.0F) *colors[i].color = colors[i].fallback;
    }

    return style;
}

f32 _nya_ui_scale_derive(const NYA_Window* window, const NYA_UIStyle* style) {
    if (style->scale > 0.0F) return style->scale;

    f32 display = window->sdl_window != nullptr ? nya_window_display_scale(window->handle) : 1.0F;
    f32 scale   = nya_max((f32)window->screen_height / style->reference_height, display);

    return nya_max(roundf(scale / NYA_UI_SCALE_STEP) * NYA_UI_SCALE_STEP, NYA_UI_SCALE_MIN);
}

_NYA_UILook _nya_ui_look_build(const NYA_UIStyle* style, f32 scale) {
    _NYA_UILook look = {
        .style       = *style,
        .margin      = roundf(style->margin * scale),
        .padding     = roundf(style->padding * scale),
        .spacing     = roundf(style->spacing * scale),
        .radius      = roundf(style->radius * scale),
        .outline     = roundf(style->outline * scale),
        .depth       = roundf(style->depth * scale),
        .pop         = roundf(style->pop * scale),
        .item_height = roundf(style->item_height * scale),
    };

    // by name every pass, since the registry can change under a hot reload.
    NYA_Font body  = nya_font_resolve(style->font[0] != '\0' ? nya_font_named(style->font) : NYA_FONT_NONE);
    NYA_Font title = style->title_font[0] != '\0' ? nya_font_resolve(nya_font_named(style->title_font)) : body;

    const f32 sizes[NYA_UI_TEXT_COUNT] = {
        [NYA_UI_TEXT_INHERIT] = style->body_size,
        [NYA_UI_TEXT_BODY]    = style->body_size,
        [NYA_UI_TEXT_SMALL]   = style->small_size,
        [NYA_UI_TEXT_TITLE]   = style->title_size,
    };

    // rasterised at the scaled size in whole points, so text is as crisp at 4K as at 720p.
    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) {
        NYA_Font face = i == NYA_UI_TEXT_TITLE ? title : body;

        look.fonts[i] = nya_font(face.path, nya_max(roundf(sizes[i] * scale), 1.0F));
        if (!nya_font_equals(look.fonts[i], face) && nya_font_sdf(face) && !nya_font_sdf(look.fonts[i])) (void)nya_font_sdf_set(look.fonts[i], true);

        look.line_heights[i] = nya_font_valid(look.fonts[i]) ? ceilf(nya_font_metrics(look.fonts[i]).line_height) : 0.0F;
    }

    return look;
}

const _NYA_UILook* _nya_ui_look(void) {
    return &_nya_ui.looks[_nya_ui.look_depth];
}

f32 _nya_ui_px(f32 value) {
    return roundf(value * _nya_ui.open->scale);
}

void _nya_ui_input_read(NYA_UI* ui) {
    _nya_ui.pointer      = nya_input_mouse_position();
    _nya_ui.pointer_down = nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.confirm_down = nya_input_action_pressed(NYA_INPUT_ACTION_CONFIRM);

    b8 input = ui->pass == NYA_UI_PASS_INPUT;

    f32x2 delta              = nya_input_mouse_position_delta();
    _nya_ui.confirm          = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CONFIRM);
    _nya_ui.cancel           = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CANCEL);
    _nya_ui.pointer_moved    = input && (delta.x != 0.0F || delta.y != 0.0F);
    _nya_ui.pointer_pressed  = input && nya_input_mouse_button_just_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.pointer_released = input && nya_input_mouse_button_just_released(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.wheel            = input ? nya_input_mouse_wheel_scroll().y : 0.0F;

    nya_memset(_nya_ui.presses, 0, sizeof(_nya_ui.presses));
    if (!input) return;

    u64 tick = nya_world()->sim_system.tick + 1;

    if (tick != _nya_ui.press_tick) {
        _nya_ui.press_tick = tick;
        _nya_ui_presses_read(ui->editing != 0);
    }

    nya_memcpy(_nya_ui.presses, _nya_ui.tick_presses, sizeof(_nya_ui.presses));

    // the menu keys are letters a field types.
    if (ui->editing != 0) nya_memset(_nya_ui.presses, 0, _NYA_UI_CARET_LEFT * sizeof(b8));
}

void _nya_ui_presses_read(b8 typing) {
    b8* fired = _nya_ui.tick_presses;
    nya_memset(fired, 0, sizeof(_nya_ui.tick_presses));

    u32 repeat = 0;

    for (u32 i = 0; i < _NYA_UI_PRESS_COUNT; i++) {
        const _NYA_UIPress* press = &_NYA_UI_PRESSES[i];

        b8 pressed = press->action != NYA_INPUT_ACTION_NONE ? nya_input_action_just_pressed(press->action) : nya_input_key_just_pressed(press->key);
        if (!pressed) continue;

        fired[i] = true;

        // an arrow fires a direction and a caret key at once: a menu repeats the direction, a field the caret.
        if (repeat == 0 || typing) repeat = i + 1;
    }

    if (repeat != 0) {
        _nya_ui.repeat_press = repeat;
        _nya_ui.repeat_s     = -NYA_UI_REPEAT_DELAY_S;
    }

    if (_nya_ui.repeat_press == 0) return;

    u32 held = _nya_ui.repeat_press - 1;
    if (fired[held]) return;

    const _NYA_UIPress* press = &_NYA_UI_PRESSES[held];

    if (!(press->action != NYA_INPUT_ACTION_NONE ? nya_input_action_pressed(press->action) : nya_input_key_pressed(press->key))) {
        _nya_ui.repeat_press = 0;
        return;
    }

    _nya_ui.repeat_s += nya_app_get()->frame_stats.delta_time_s;
    if (_nya_ui.repeat_s < NYA_UI_REPEAT_INTERVAL_S) return;

    _nya_ui.repeat_s -= NYA_UI_REPEAT_INTERVAL_S;
    fired[held]       = true;
}

u64 _nya_ui_id(u64 scope, NYA_ConstCString label) {
    u64 parts[2] = { scope, nya_hash_fnv1a(label) };
    u64 id       = nya_hash_fnv1a(parts, sizeof(parts));

    return id != 0 ? id : 1;
}

u64 _nya_ui_id_at(u64 key, u32 index) {
    u64 parts[2] = { key, (u64)index + 1 };
    u64 id       = nya_hash_fnv1a(parts, sizeof(parts));

    return id != 0 ? id : 1;
}

u32 _nya_ui_panel_claim(u64 id) {
    u32 free  = U32_MAX;
    u32 stale = U32_MAX;

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        _NYA_UIPanelState* state = &_nya_ui.panels[i];

        if (state->id == id) return i;
        if (state->id == 0 && free == U32_MAX) free = i;
        if (state->id != 0 && state->pass != _nya_ui.pass_serial && (stale == U32_MAX || state->pass < _nya_ui.panels[stale].pass)) stale = i;
    }

    if (free != U32_MAX) {
        _nya_ui.panel_count += 1;
    } else if (stale != U32_MAX) {
        free = stale;
    } else {
        return U32_MAX;
    }

    _nya_ui.panels[free] = (_NYA_UIPanelState){ .id = id };
    return free;
}

_NYA_UILayout* _nya_ui_layout_push(void) {
    nya_assert(_nya_ui.depth < NYA_UI_DEPTH_MAX, "containers nest deeper than NYA_UI_DEPTH_MAX");

    return &_nya_ui.layouts[_nya_ui.depth++];
}

f32 _nya_ui_extent(NYA_UISize size, f32 measured, f32 available) {
    f32 extent = measured;

    if (size.kind == NYA_UI_SIZE_FIXED) extent = _nya_ui_px(size.value);
    if (size.kind == NYA_UI_SIZE_GROW) extent = roundf(available * size.value);

    extent = nya_max(extent, _nya_ui_px(size.min));
    if (size.max > 0.0F) extent = nya_min(extent, _nya_ui_px(size.max));

    return nya_min(extent, available);
}

NYA_UISize _nya_ui_next_size(const _NYA_UILayout* layout, NYA_UISize own) {
    if (_nya_ui.next_set) return _nya_ui.next;
    if (own.kind != NYA_UI_SIZE_AUTO) return own;

    return layout->children;
}

f32 _nya_ui_share(const _NYA_UILayout* layout, f32 placed, f32 weight) {
    if (layout->grow_total <= 0.0F) return -1.0F;

    // cumulative, so rounding each share cannot leave the row a pixel short or long.
    f32 start = roundf(layout->grow_space * nya_min(placed / layout->grow_total, 1.0F));
    f32 end   = roundf(layout->grow_space * nya_min((placed + weight) / layout->grow_total, 1.0F));

    return end - start;
}

f32 _nya_ui_text_room(const _NYA_UILayout* layout) {
    // the room, not the width: a column that fits its content would otherwise wrap to what it measured last pass.
    if (layout->main == 1) return layout->room.x;

    NYA_UISize size = _nya_ui_next_size(layout, (NYA_UISize){ 0 });

    if (size.kind == NYA_UI_SIZE_FIXED) return _nya_ui_px(size.value);

    if (size.kind == NYA_UI_SIZE_GROW) {
        f32 share = _nya_ui_share(layout, layout->grow_placed, size.value);
        if (share >= 0.0F) return share;
    }

    // what is left of the row.
    f32 gap = layout->count > 0 ? layout->gap : 0.0F;
    return nya_max(layout->room.x - layout->used - gap, 0.0F);
}

NYA_Rectf _nya_ui_place(NYA_UISize own, f32x2 natural, b8 fill) {
    _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    u32        main  = layout->main;
    u32        cross = 1 - main;
    NYA_UISize size  = _nya_ui_next_size(layout, own);

    _nya_ui.next_set = false;

    f32 along = natural[main];

    if (size.kind == NYA_UI_SIZE_FIXED) along = _nya_ui_px(size.value);

    if (size.kind == NYA_UI_SIZE_GROW) {
        // a container seen for the first time has no weights yet, and fits until it does.
        f32 share = _nya_ui_share(layout, layout->grow_placed, size.value);
        if (share >= 0.0F) along = share;

        layout->grow_placed += size.value;
        layout->grow        += size.value;
    }

    along = nya_max(along, _nya_ui_px(size.min));
    if (size.max > 0.0F) along = nya_min(along, _nya_ui_px(size.max));
    along = roundf(along);

    if (size.kind != NYA_UI_SIZE_GROW) layout->fixed += along;

    f32 extent = layout->extent[cross];
    f32 across = fill && extent > 0.0F ? extent : roundf(natural[cross]);
    f32 gap    = layout->count > 0 ? layout->gap : 0.0F;

    f32x2 position  = { 0.0F, 0.0F };
    position[main]  = layout->origin[main] + layout->used + gap;
    position[cross] = layout->origin[cross] + (extent > 0.0F && !fill ? roundf((extent - across) * (f32)layout->align * 0.5F) : 0.0F);

    f32x2 taken  = { 0.0F, 0.0F };
    taken[main]  = along;
    taken[cross] = across;

    layout->used   += gap + along;
    layout->across  = nya_max(layout->across, roundf(natural[cross]));
    layout->count  += 1;

    return (NYA_Rectf){ position.x, position.y, taken.x, taken.y };
}

_NYA_UIWidget _nya_ui_widget(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, b8 horizontal) {
    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    u64 id = _nya_ui_id(layout->scope, label);

    // left out of focus entirely, so the keys step over it.
    if (_nya_ui.disabled > 0) return (_NYA_UIWidget){ .id = id, .disabled = true };

    if (_nya_ui.widget_count >= NYA_UI_WIDGETS_MAX) return (_NYA_UIWidget){ .refused = true };

    if (ui->pass == NYA_UI_PASS_INPUT) {
        for (u32 i = 0; i < _nya_ui.widget_count; i++) {
            nya_assert(_nya_ui.widgets[i] != id, "two widgets in one panel share the label '%s'", label);
        }
    }

    // an empty focus goes to the first widget as it is declared, so a menu's first pass can already confirm.
    if (ui->focus == 0) _nya_ui_focus_set(ui, id);

    u32 index                        = _nya_ui.widget_count++;
    _nya_ui.widgets[index]           = id;
    _nya_ui.widget_groups[index]     = layout->group != U32_MAX ? layout->group : index;
    _nya_ui.widget_horizontal[index] = horizontal;
    if (ui->focus == id) _nya_ui.focus_found = index;

    _NYA_UIWidget widget = { .id = id };

    if (ui->pass == NYA_UI_PASS_INPUT) {
        b8 inside = nya_rect_contains(rect, _nya_ui.pointer) && nya_rect_contains(layout->clip, _nya_ui.pointer);

        // hover moves the same focus as the keys, so the two never disagree. only on movement, so a resting pointer
        // does not take focus back from the keys, and not while typing, where confirm belongs to the field.
        if (inside && (_nya_ui.pointer_pressed || (_nya_ui.pointer_moved && ui->editing == 0))) {
            _nya_ui_focus_set(ui, id);
            _nya_ui.focus_found = index;
        }

        if (inside && _nya_ui.pointer_pressed) ui->active = id;

        widget.activated = (ui->focus == id && _nya_ui.confirm) || (ui->active == id && _nya_ui.pointer_released && inside);
    }

    widget.focused = ui->focus == id;
    widget.held    = (ui->active == id && _nya_ui.pointer_down) || (widget.focused && _nya_ui.confirm_down);
    widget.focus   = widget.focused ? 1.0F : 0.0F;
    widget.press   = widget.held ? 1.0F : 0.0F;

    if (ui->pass == NYA_UI_PASS_DRAW && _nya_ui_look()->style.transition_s > 0.0F) _nya_ui_animate(&widget);

    if (widget.focused && ui->reveal) {
        _nya_ui_reveal(rect);
        ui->reveal = false;
    }

    return widget;
}

void _nya_ui_focus_set(NYA_UI* ui, u64 id) {
    if (ui->focus == id) return;

    ui->focus           = id;
    ui->focus_changed_s = nya_app_uptime_s();
}

void _nya_ui_reveal(NYA_Rectf rect) {
    for (u32 depth = _nya_ui.depth; depth > 1; depth--) {
        const _NYA_UILayout* layout = &_nya_ui.layouts[depth - 1];
        if (!layout->scrolls) continue;

        _NYA_UIPanelState* state = &_nya_ui.panels[layout->panel];

        // with room for the shadow under it; panel_end clamps what this overshoots.
        f32 top    = layout->origin.y + layout->scroll;
        f32 bottom = top + layout->extent.y;
        f32 below  = rect.y + rect.height + _nya_ui_look()->depth;

        if (rect.y < top) state->scroll -= top - rect.y;
        if (below > bottom && rect.y >= top) state->scroll += below - bottom;

        return;
    }
}

b8 _nya_ui_field(NYA_UI* ui, _NYA_UIWidget widget, b8 start, NYA_Rectf owner, NYA_Rectf box, char* buffer, u32 capacity) {
    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();
    const NYA_UIStyle*   style  = &look->style;

    NYA_Font font = look->fonts[layout->text];
    f32      line = look->line_heights[layout->text];

    // a buffer the caller filled without a terminator in range reads as full.
    buffer[capacity - 1] = '\0';

    u32 length  = (u32)strlen(buffer);
    b8  changed = false;
    b8  editing = ui->editing == widget.id;

    if (ui->pass == NYA_UI_PASS_INPUT && editing) {
        _nya_ui.editing_seen = true;

        const b8*        press = _nya_ui.presses;
        NYA_ConstCString typed = nya_input_text();
        u32              caret = nya_min(ui->caret, length);

        u32 typed_length = (u32)strlen(typed);
        u32 fits         = nya_min(typed_length, capacity - 1 - length);

        // cut on a codepoint boundary when the buffer fills.
        while (fits > 0 && fits < typed_length && ((u8)typed[fits] & 0xC0) == 0x80) fits--;

        if (fits > 0) {
            nya_memmove(buffer + caret + fits, buffer + caret, length - caret + 1);
            nya_memcpy(buffer + caret, typed, fits);

            caret   += fits;
            length  += fits;
            changed  = true;
        }

        u32 previous = caret > 0 ? caret - 1 : 0;
        while (previous > 0 && ((u8)buffer[previous] & 0xC0) == 0x80) previous--;

        u32 next = caret < length ? caret + nya_min(nya_utf8_length(buffer + caret), length - caret) : caret;

        if (press[_NYA_UI_BACKSPACE] && caret > 0) {
            nya_memmove(buffer + previous, buffer + caret, length - caret + 1);
            caret   = previous;
            changed = true;
        } else if (press[_NYA_UI_DELETE] && next > caret) {
            nya_memmove(buffer + caret, buffer + next, length - next + 1);
            changed = true;
        } else if (press[_NYA_UI_CARET_LEFT]) {
            caret = previous;
        } else if (press[_NYA_UI_CARET_RIGHT]) {
            caret = next;
        }

        if (nya_input_key_just_pressed(NYA_KEY_HOME)) caret = 0;
        if (nya_input_key_just_pressed(NYA_KEY_END)) caret = (u32)strlen(buffer);

        ui->caret = caret;

        b8 returned = nya_input_key_just_pressed(NYA_KEY_RETURN) || nya_input_key_just_pressed(NYA_KEY_KP_ENTER);
        b8 outside  = _nya_ui.pointer_pressed && !nya_rect_contains(owner, _nya_ui.pointer);

        // a confirm that typed something is the space bar.
        if (returned || (_nya_ui.confirm && typed_length == 0) || _nya_ui.cancel || outside) {
            _nya_ui_typing_stop(ui);
            _nya_ui.cancel = false;
            editing        = false;
        }
    } else if (ui->pass == NYA_UI_PASS_INPUT && start && !widget.disabled) {
        _nya_ui_typing_start(ui, widget.id, length, box);
    }

    if (!_nya_ui_drawn(box)) return changed;

    NYA_Color color = _nya_ui_color(&style->text, widget);
    _nya_ui_track_draw(ui, box, look->radius, style->track);

    f32 caret_x = 0.0F;
    f32 mark    = _nya_ui_px(2.0F);

    if (editing) {
        char prefix[NYA_UI_TEXT_INPUT_MAX];
        u32  caret = nya_min(ui->caret, (u32)strlen(buffer));

        nya_memcpy(prefix, buffer, caret);
        prefix[caret] = '\0';
        caret_x       = roundf(nya_font_width(font, prefix));

        nya_render2d_rect(ui->window, box.x, box.y + box.height - mark, box.width, mark, style->accent);
    }

    // scrolled so the caret stays in the box, and clipped to it.
    f32 margin = roundf(look->padding * 0.5F);
    f32 shift  = nya_max(caret_x - (box.width - (margin * 2.0F)), 0.0F);
    f32 x      = box.x + margin - shift;
    f32 y      = roundf(box.y + ((box.height - line) * 0.5F));

    _nya_ui_scissor(ui, nya_rect_intersection(layout->clip, box));
    nya_font_draw(ui->window, font, buffer, x, y, color);

    if (editing) {
        // the IME's unfinished text sits at the caret, dimmed, and the caret after it.
        NYA_ConstCString composing = nya_input_text_composition();

        if (composing[0] != '\0') {
            nya_font_draw(ui->window, font, composing, x + caret_x, y, style->text_dim);
            caret_x += roundf(nya_font_width(font, composing));
        }

        if (fmod(nya_app_uptime_s(), NYA_UI_CARET_BLINK_S * 2.0) < NYA_UI_CARET_BLINK_S) nya_render2d_rect(ui->window, x + caret_x, y, mark, line, color);
    }

    _nya_ui_scissor(ui, layout->clip);

    return changed;
}

void _nya_ui_animate(_NYA_UIWidget* widget) {
    const NYA_UIStyle* style = &_nya_ui_look()->style;
    _NYA_UIAnimation*  entry = &_nya_ui.animations[widget->id % NYA_UI_ANIMATIONS_MAX];
    f64                now   = nya_app_uptime_s();

    // a slot another widget held starts where this one already is, which only snaps that widget's transition.
    if (entry->id != widget->id) *entry = (_NYA_UIAnimation){ .id = widget->id, .time_s = now, .focus = widget->focus, .press = widget->press };

    f32 step      = (f32)(now - entry->time_s) / style->transition_s;
    entry->time_s = now;
    entry->focus  = widget->focus > entry->focus ? nya_min(entry->focus + step, widget->focus) : nya_max(entry->focus - step, widget->focus);
    entry->press  = widget->press > entry->press ? nya_min(entry->press + step, widget->press) : nya_max(entry->press - step, widget->press);

    widget->focus = nya_ease(style->easing, entry->focus);
    widget->press = nya_ease(style->easing, entry->press);
}

void _nya_ui_typing_start(NYA_UI* ui, u64 id, u32 caret, NYA_Rectf field) {
    ui->editing          = id;
    ui->caret            = caret;
    _nya_ui.editing_seen = true;

    nya_input_text_begin(ui->window->handle);
    nya_input_text_area_set(ui->window->handle, field.x, field.y, field.width, field.height);
}

void _nya_ui_typing_stop(NYA_UI* ui) {
    ui->editing = 0;

    nya_input_text_end();
}

b8 _nya_ui_drawing(void) {
    return _nya_ui.open->pass == NYA_UI_PASS_DRAW && !_nya_ui.layouts[_nya_ui.depth - 1].hidden;
}

b8 _nya_ui_drawn(NYA_Rectf rect) {
    if (!_nya_ui_drawing()) return false;

    // a list scrolled far past a widget draws nothing for it.
    const _NYA_UILook* look  = _nya_ui_look();
    NYA_Rectf          reach = nya_rect_expand(rect, look->pop + look->depth + 1.0F);

    return nya_rect_overlaps(reach, _nya_ui.layouts[_nya_ui.depth - 1].clip);
}

void _nya_ui_scissor(const NYA_UI* ui, NYA_Rectf clip) {
    const NYA_Rectf* screen = &_nya_ui.layouts[0].clip;

    if (clip.x == screen->x && clip.y == screen->y && clip.width == screen->width && clip.height == screen->height) {
        nya_render2d_scissor_end(ui->window);
        return;
    }

    nya_render2d_scissor_begin(ui->window, clip.x, clip.y, clip.width, clip.height);
}

f32 _nya_ui_item_height(const _NYA_UILayout* layout) {
    const _NYA_UILook* look = _nya_ui_look();

    return look->item_height > 0.0F ? look->item_height : look->line_heights[layout->text] + roundf(look->padding * 1.5F);
}

NYA_Color _nya_ui_color(const NYA_UIStateColors* colors, _NYA_UIWidget widget) {
    if (widget.disabled) return colors->disabled;

    return nya_color_mix(nya_color_mix(colors->normal, colors->focused, widget.focus), colors->pressed, widget.press);
}

NYA_UISkin _nya_ui_skin(const NYA_UIStateSkins* skins, _NYA_UIWidget widget) {
    const NYA_UISkin* skin = &skins->normal;

    if (widget.disabled) {
        skin = &skins->disabled;
    } else if (widget.held) {
        skin = &skins->pressed;
    } else if (widget.focused) {
        skin = &skins->focused;
    }

    if (skin->texture[0] != '\0' || skin->source_width > 0.0F) return *skin;

    NYA_UISkin fallback = skins->normal;
    NYA_Color  tint     = skin->tint;
    if (tint.r != 0.0F || tint.g != 0.0F || tint.b != 0.0F || tint.a != 0.0F) fallback.tint = tint;

    return fallback;
}

b8 _nya_ui_skin_draw(const NYA_UI* ui, const NYA_UISkin* skin, NYA_Rectf rect, NYA_Color tint) {
    // a region alone is cut from the panel's sheet, so one texture skins everything.
    NYA_ConstCString texture = skin->texture;
    if (texture[0] == '\0' && skin->source_width > 0.0F && skin->source_height > 0.0F) texture = _nya_ui_look()->style.panel_skin.texture;

    if (texture[0] == '\0') return false;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture);

    // loaded on first use, and drawn flat until it is.
    if (asset == nullptr) {
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = (NYA_CString)texture });
        return false;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return false;

    NYA_Color own = skin->tint;
    b8        set = own.r != 0.0F || own.g != 0.0F || own.b != 0.0F || own.a != 0.0F;

    nya_render2d_nine_slice(
        ui->window,
        texture,
        (NYA_NineSlice){
            .left          = skin->left,
            .right         = skin->right,
            .top           = skin->top,
            .bottom        = skin->bottom,
            .x             = rect.x,
            .y             = rect.y,
            .width         = rect.width,
            .height        = rect.height,
            .source_x      = skin->source_x,
            .source_y      = skin->source_y,
            .source_width  = skin->source_width,
            .source_height = skin->source_height,
            .scale         = ui->scale,
            .fill          = skin->tile ? NYA_NINE_SLICE_TILE : NYA_NINE_SLICE_STRETCH,
            .hollow        = skin->hollow,
            .tint          = set ? own : tint,
        }
    );

    return true;
}

NYA_Rectf _nya_ui_body_draw(NYA_UI* ui, NYA_Rectf rect, _NYA_UIWidget widget) {
    const _NYA_UILook* look  = _nya_ui_look();
    const NYA_UIStyle* style = &look->style;

    if (widget.focused && look->pop > 0.0F) {
        // grows at once and settles back, ease out.
        f32 t    = nya_clamp((f32)(nya_app_uptime_s() - ui->focus_changed_s) / NYA_UI_POP_S, 0.0F, 1.0F);
        f32 grow = roundf(look->pop * (1.0F - t) * (1.0F - t));

        rect = (NYA_Rectf){ rect.x - grow, rect.y - grow, rect.width + (grow * 2.0F), rect.height + (grow * 2.0F) };
    }

    // held, the body sinks onto its shadow.
    if (look->depth > 0.0F && !widget.held) nya_render2d_rect_rounded(ui->window, rect.x, rect.y + look->depth, rect.width, rect.height, look->radius, style->ink);
    if (widget.held) rect.y += look->depth;

    NYA_UISkin skin = _nya_ui_skin(&style->button_skin, widget);

    NYA_Color fill = _nya_ui_color(&style->button, widget);

    if (!_nya_ui_skin_draw(ui, &skin, rect, fill)) {
        nya_render2d_rect_rounded(ui->window, rect.x, rect.y, rect.width, rect.height, look->radius, fill);
        if (look->outline > 0.0F) nya_render2d_rect_rounded_outline(ui->window, rect.x, rect.y, rect.width, rect.height, look->radius, look->outline, style->ink);
    }

    // clear of the rounded corners, fading with the focus.
    if (widget.focus > 0.0F) {
        f32       bar    = _nya_ui_px(NYA_UI_FOCUS_BAR);
        NYA_Color accent = { style->accent.r, style->accent.g, style->accent.b, style->accent.a * widget.focus };

        nya_render2d_rect(ui->window, rect.x + look->outline, rect.y + look->radius, bar, nya_max(rect.height - (look->radius * 2.0F), 0.0F), accent);
    }

    return rect;
}

void _nya_ui_track_draw(NYA_UI* ui, NYA_Rectf rect, f32 radius, NYA_Color color) {
    const _NYA_UILook* look = _nya_ui_look();

    if (rect.width <= 0.0F || _nya_ui_skin_draw(ui, &look->style.track_skin, rect, color)) return;

    nya_render2d_rect_rounded(ui->window, rect.x, rect.y, rect.width, rect.height, radius, color);
    if (look->outline > 0.0F) nya_render2d_rect_rounded_outline(ui->window, rect.x, rect.y, rect.width, rect.height, radius, look->outline, look->style.ink);
}

void _nya_ui_text_draw(NYA_UI* ui, const _NYA_UILayout* layout, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color) {
    const _NYA_UILook* look = _nya_ui_look();

    f32 x = roundf(rect.x + ((rect.width - width) * (f32)align * 0.5F));
    f32 y = roundf(rect.y + ((rect.height - look->line_heights[layout->text]) * 0.5F));

    nya_font_draw(ui->window, look->fonts[layout->text], text, x, y, color);
}
