/**
 * @file ui_code_editor.c
 *
 * A multi-line text editor built from the primitives already in the module: a line-number gutter and a monospace
 * grid of text, a caret, a selection, and vertical and horizontal scrolling. The gutter and the text are labels,
 * the selection, the caret and the current line are the flat fills a rule is drawn from, and everything is cut to
 * the box — so the whole thing draws on a GPU, in a terminal and through the recorder that tests it, and follows a
 * restyle like the rest of the UI. Nothing here names a colour of the backend's. See ui.h.
 *
 * Why it is shaped this way
 *
 * - The editing is a field's, lifted to lines. A field already knows how to insert on a codepoint boundary, move
 *   by character and by word, and keep a caret and a selection in a caller's buffer; every one of those helpers
 *   works on the flat bytes and knows nothing of newlines, so the editor calls them unchanged. A newline is a byte
 *   like any other, which is why enter is an inserted "\n" and backspace at the start of a line is the same erase
 *   as backspace anywhere — it removes the newline before the caret, and the two lines are one. The editor adds
 *   only what a second dimension needs: up and down between lines, home and end within one, and a paste that keeps
 *   its newlines where a one-line field flattens them.
 * - The caret and the selection are not the editor's own state. They live in the one-at-a-time keyboard the fields
 *   use, so one editor or one field is typed into at a time and a click hands the keyboard over the same way. What
 *   the caller owns and the widget writes is the buffer and the scroll; what it reads back is where the caret and
 *   the view ended up, the way a node canvas reports its gestures.
 * - The text is measured a line at a time by terminating it in place. A label needs a run with no newline in it and
 *   a terminator after it; rather than copy each line out, the draw writes a '\0' over the line's newline, emits the
 *   label, and puts the byte back before the next line. The same trick measures a caret's column without the
 *   256-byte cap the field's own measure carries, since a line here may be longer than a field ever is.
 *
 * Syntax highlighting is a follow-up: every line's text goes out as one label in one colour. Colouring runs would
 * split that label into several, each its own colour, without touching the layout, the scrolling or the editing.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-ui/ui_internal.h"


// PRIVATE API DECLARATION

/** How many lines `buffer` holds: one more than the newlines in its first `length` bytes. */
NYA_INTERNAL u32 _nya_ui_code_lines(NYA_ConstCString buffer, u32 length) __attr_no_discard;

/** The byte range [`*start`, `*end`) of line `index`, `*end` on the newline or the terminator that ends it. */
NYA_INTERNAL void _nya_ui_code_line_bounds(NYA_ConstCString buffer, u32 length, u32 index, u32* start, u32* end);

/** The line `offset` sits on, and its column in codepoints from that line's start. */
NYA_INTERNAL void _nya_ui_code_locate(NYA_ConstCString buffer, u32 offset, u32* line, u32* column);

/** How wide the bytes [`start`, `upto`) of `buffer` draw at `role`, measured by terminating the run in place. */
NYA_INTERNAL f32 _nya_ui_code_prefix(NYA_UIText role, char* buffer, u32 start, u32 upto) __attr_no_discard;

/** The codepoint boundary in line [`start`, `end`) nearest `x` pixels from the line's start. */
NYA_INTERNAL u32 _nya_ui_code_offset_at(NYA_UIText role, char* buffer, u32 start, u32 end, f32 x) __attr_no_discard;

/** Puts the selection, newlines and all, on the system clipboard by terminating it in place. False when empty or refused. */
NYA_INTERNAL b8 _nya_ui_code_copy(const NYA_UI* ui, char* buffer, u32 length) __attr_no_discard;

/** The keys that edit `buffer` while the editor has the keyboard, everything but the plain typed text. True when it changed. */
NYA_INTERNAL b8 _nya_ui_code_keys(NYA_UI* ui, char* buffer, u32 capacity, u32* length, u32 lines, NYA_UICodeEditor* editor);


// THE WIDGET

b8 nya_ui_code_editor(NYA_UI* ui, NYA_ConstCString id, char* buffer, u32 capacity, NYA_UICodeEditor* editor) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr, "a code editor is named, so its size and its scroll survive the list it is in being reordered");
    nya_assert(buffer != nullptr && editor != nullptr);
    nya_assert(capacity > 0 && capacity <= NYA_UI_CODE_EDITOR_MAX, "a code editor edits 1 to NYA_UI_CODE_EDITOR_MAX bytes, got %u", capacity);

    _NYA_UILayout*    layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook* look   = _nya_ui_look();
    const NYA_UIStyle style  = look->style;
    NYA_UIText        role   = layout->text;

    // a buffer the caller filled without a terminator in range reads as full, so length never runs past it.
    buffer[capacity - 1] = '\0';
    u32 length = (u32)strlen(buffer);
    u32 lines  = _nya_ui_code_lines(buffer, length);

    f32 line   = look->line_heights[role];
    f32 margin = roundf(look->padding * 0.5F);
    f32 cell   = nya_max(_nya_ui_text_width(role, "0"), 1.0F);

    // the gutter is wide enough for the last line's number, never fewer than a couple of digits so it does not jump.
    u32 digits = NYA_UI_CODE_EDITOR_DIGITS;
    for (u32 n = lines; n >= 10; n /= 10) digits++;
    f32 gutter_w = roundf(((f32)digits * cell) + (margin * 2.0F));

    // fills its container the way a node canvas does; the natural size is only the fallback when nothing sizes it.
    f32x2     natural = { gutter_w + (cell * 40.0F), ((f32)nya_min(lines, 12U) * line) + line };
    NYA_Rectf rect    = _nya_ui_place(nya_ui_grow(1), natural, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, id, rect, false);
    if (widget.refused) return false;

    NYA_Rectf gutter = { rect.x, rect.y, gutter_w, rect.height };
    NYA_Rectf text   = { rect.x + gutter_w, rect.y, nya_max(rect.width - gutter_w, 1.0F), rect.height };
    f32       inner  = text.x + margin;                             // where a line's first glyph sits before scrolling
    f32       room   = nya_max(text.width - (margin * 2.0F), 1.0F); // the width a line has before it scrolls
    u32       fit    = (u32)nya_max(floorf(text.height / line), 1.0F);

    b8 editing = ui->editing == widget.id;
    b8 changed = false;
    b8 moved   = false; // whether the caret moved or the text changed this pass, so the view follows only then

    if (ui->pass == NYA_UI_PASS_INPUT && editing) {
        _nya_ui.editing_seen = true;

        // the caller owns the buffer and may have shortened it since the last pass, so both offsets are pulled back in.
        ui->caret  = nya_min(ui->caret, length);
        ui->select = nya_min(ui->select, length);

        NYA_ConstCString typed = nya_input_text();
        u32              size  = (u32)strlen(typed);

        // ctrl is a command, not a character: its shortcuts are read below and whatever the OS sent is dropped.
        if ((nya_input_modifiers() & NYA_KEYMOD_CTRL) != 0) size = 0;

        if (size > 0) {
            u32 dropped = _nya_ui_field_selection_erase(ui, buffer, length);
            b8  removed = dropped != length;

            u32 caret = ui->caret;
            u32 grown = _nya_ui_field_insert(buffer, dropped, capacity, &caret, typed);

            _nya_ui_field_caret_set(ui, caret, false);

            changed = removed || grown != dropped;
            length  = grown;
            moved   = true;

            editor->_goal_set = false;
        }

        b8 edited = _nya_ui_code_keys(ui, buffer, capacity, &length, lines, editor);
        changed |= edited;
        moved   |= edited || _nya_ui.presses[_NYA_UI_CARET_LEFT] || _nya_ui.presses[_NYA_UI_CARET_RIGHT] ||
                 nya_input_key_just_pressed(NYA_KEY_UP) || nya_input_key_just_pressed(NYA_KEY_DOWN) ||
                 nya_input_key_just_pressed(NYA_KEY_HOME) || nya_input_key_just_pressed(NYA_KEY_END);

        // the pointer, when the editor already has the keyboard: a press sets the caret, a drag selects, a second press on the same spot takes the word under it.
        if (_nya_ui.pointer_pressed && !layout->covered && nya_rect_contains(text, _nya_ui.pointer)) {
            u32 li = (u32)nya_clamp(floorf((_nya_ui.pointer.y - rect.y + editor->scroll.y) / line), 0.0F, (f32)(lines - 1));

            u32 ls, le;
            _nya_ui_code_line_bounds(buffer, length, li, &ls, &le);

            f32 local  = _nya_ui.pointer.x - inner + editor->scroll.x;
            u32 offset = _nya_ui_code_offset_at(role, buffer, ls, le, local);

            f64 now  = nya_app_uptime_s();
            b8  again = ui->click_id == widget.id && now - ui->click_s < NYA_UI_DOUBLE_CLICK_S;

            ui->click_id = widget.id;
            ui->click_s  = now;

            if (again) {
                ui->select = _nya_ui_field_word_start(buffer, offset);
                ui->caret  = _nya_ui_field_word_end(buffer, length, offset);
            } else {
                _nya_ui_field_caret_set(ui, offset, (nya_input_modifiers() & NYA_KEYMOD_SHIFT) != 0);
            }

            ui->dragging      = true;
            editor->_goal_set = false;
            moved             = true;
        } else if (ui->active == widget.id && ui->dragging && _nya_ui.pointer_down && _nya_ui.pointer_moved) {
            u32 li = (u32)nya_clamp(floorf((_nya_ui.pointer.y - rect.y + editor->scroll.y) / line), 0.0F, (f32)(lines - 1));

            u32 ls, le;
            _nya_ui_code_line_bounds(buffer, length, li, &ls, &le);

            f32 local = _nya_ui.pointer.x - inner + editor->scroll.x;
            _nya_ui_field_caret_set(ui, _nya_ui_code_offset_at(role, buffer, ls, le, local), true);
            moved = true;
        }

        // cancel or a press outside stops typing; enter does not, since a newline is a character here.
        b8 outside = _nya_ui.pointer_pressed && (layout->covered || !nya_rect_contains(rect, _nya_ui.pointer));

        if (_nya_ui.cancel || outside) {
            _nya_ui_typing_stop(ui);
            _nya_ui.cancel = false;
            editing        = false;
        }
    } else if (ui->pass == NYA_UI_PASS_INPUT && widget.activated && !widget.disabled) {
        // opened by a click: the caret lands where the click did; opened by confirm: at the end, ready to append.
        u32 caret = length;

        if (_nya_ui.pointer_released && nya_rect_contains(text, _nya_ui.pointer)) {
            u32 li = (u32)nya_clamp(floorf((_nya_ui.pointer.y - rect.y + editor->scroll.y) / line), 0.0F, (f32)(lines - 1));

            u32 ls, le;
            _nya_ui_code_line_bounds(buffer, length, li, &ls, &le);

            caret = _nya_ui_code_offset_at(role, buffer, ls, le, _nya_ui.pointer.x - inner + editor->scroll.x);
        }

        _nya_ui_typing_start(ui, widget.id, caret, text);
        editing           = true;
        editor->_goal_set = false;
        moved             = true;
    }

    // this pass may have inserted or removed a newline, so the line count is taken again before it is read back.
    lines = _nya_ui_code_lines(buffer, length);

    // where the caret is, for the read-back and for following it with the view.
    u32 caret_line = 0, caret_column = 0;
    if (editing) _nya_ui_code_locate(buffer, ui->caret, &caret_line, &caret_column);

    // the view: the wheel scrolls it, and an edit or move keeps the caret inside it; bounds are clamped last, so neither the wheel nor a reveal can leave the content.
    f32 max_y = nya_max(((f32)lines * line) - text.height, 0.0F);

    if (ui->pass == NYA_UI_PASS_INPUT && !layout->covered && nya_rect_contains(rect, _nya_ui.pointer)) {
        if (_nya_ui.wheel != 0.0F) {
            editor->scroll.y -= _nya_ui.wheel * NYA_UI_SCROLL_STEP;
            _nya_ui.wheel     = 0.0F;
        }

        if (_nya_ui.wheel_x != 0.0F) {
            editor->scroll.x -= _nya_ui.wheel_x * NYA_UI_SCROLL_STEP;
            _nya_ui.wheel_x   = 0.0F;
        }
    }

    if (ui->pass == NYA_UI_PASS_INPUT && editing && moved) {
        u32 ls, le;
        _nya_ui_code_line_bounds(buffer, length, caret_line, &ls, &le);

        f32 top    = (f32)caret_line * line;
        f32 caretx = _nya_ui_code_prefix(role, buffer, ls, ui->caret);

        if (top < editor->scroll.y) editor->scroll.y = top;
        if (top + line > editor->scroll.y + text.height) editor->scroll.y = top + line - text.height;

        if (caretx < editor->scroll.x) editor->scroll.x = caretx;
        if (caretx > editor->scroll.x + room) editor->scroll.x = caretx - room;
    }

    editor->scroll.y = nya_clamp(editor->scroll.y, 0.0F, max_y);
    editor->scroll.x = nya_max(editor->scroll.x, 0.0F);

    u32 first = (u32)floorf(editor->scroll.y / line);

    // the read-back the caller reads to follow the caret and the view, or to write its own status line.
    editor->focused       = editing;
    editor->changed       = changed;
    editor->lines         = lines;
    editor->cursor_line   = caret_line;
    editor->cursor_column = caret_column;
    editor->first_line    = first;
    editor->visible_lines = fit;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf saved_clip  = layout->clip;
        NYA_Rectf editor_clip = nya_rect_intersection(saved_clip, rect);
        NYA_Rectf text_clip   = nya_rect_intersection(saved_clip, text);

        NYA_Color selection = style.accent;
        selection.a        *= NYA_UI_SELECTION_ALPHA;

        NYA_Color current = style.accent;
        current.a        *= NYA_UI_SELECTION_ALPHA * 0.4F;

        u32 sel_lo = editing ? nya_min(ui->caret, ui->select) : 0;
        u32 sel_hi = editing ? nya_max(ui->caret, ui->select) : 0;

        // the two backgrounds: the text field in the panel colour, the gutter in the track colour beside it.
        layout->clip                = editor_clip;
        NYA_UIWidgetDraw text_back   = { .kind = NYA_UI_WIDGET_STRIPE, .rect = text, .color = style.panel };
        NYA_UIWidgetDraw gutter_back = { .kind = NYA_UI_WIDGET_STRIPE, .rect = gutter, .color = style.track };
        _nya_ui_draw(ui, &text_back);
        _nya_ui_draw(ui, &gutter_back);

        for (u32 i = first; i <= first + fit && i < lines; i++) {
            f32       y         = roundf(rect.y + ((f32)i * line) - editor->scroll.y);
            NYA_Rectf line_rect = { text.x, y, text.width, line };

            u32 ls, le;
            _nya_ui_code_line_bounds(buffer, length, i, &ls, &le);

            layout->clip = text_clip;

            // the band behind the caret's line, when the caller asked for one.
            if (editing && editor->highlight_line && i == caret_line) {
                NYA_UIWidgetDraw band = { .kind = NYA_UI_WIDGET_STRIPE, .rect = line_rect, .color = current };
                _nya_ui_draw(ui, &band);
            }

            // the selection on this line, out to the edge when it runs past the line's end into the next.
            if (editing && sel_hi > sel_lo && sel_lo <= le && sel_hi > ls) {
                u32 from = nya_max(sel_lo, ls);
                u32 to   = nya_min(sel_hi, le);

                f32 x0 = inner - editor->scroll.x + _nya_ui_code_prefix(role, buffer, ls, from);
                f32 x1 = sel_hi > le ? text.x + text.width : inner - editor->scroll.x + _nya_ui_code_prefix(role, buffer, ls, to);

                NYA_Rectf highlight = { roundf(x0), y, nya_max(roundf(x1 - x0), 1.0F), line };
                NYA_UIWidgetDraw sel = { .kind = NYA_UI_WIDGET_RULE, .rect = highlight, .color = selection };
                _nya_ui_draw(ui, &sel);
            }

            // the line's text, one label, scrolled left under the gutter. Terminated in place so it carries no newline.
            char saved  = buffer[le];
            buffer[le]  = '\0';

            NYA_UIWidgetDraw text_line = {
                .kind     = NYA_UI_WIDGET_LABEL,
                .rect     = { roundf(inner - editor->scroll.x), y, room + editor->scroll.x, line },
                .label    = buffer + ls,
                .color    = style.text.normal,
                .as_label = { .room = room + editor->scroll.x, .overflow = NYA_UI_OVERFLOW_VISIBLE, .align = NYA_UI_ALIGN_START },
            };

            _nya_ui_draw(ui, &text_line);
            buffer[le] = saved;

            // the caret, a thin fill at its column, drawn solid so a test sees it whatever the frame's clock.
            if (editing && ui->caret >= ls && ui->caret <= le && ui->select == ui->caret) {
                f32       cx    = roundf(inner - editor->scroll.x + _nya_ui_code_prefix(role, buffer, ls, ui->caret));
                NYA_Rectf caret = { cx, y, nya_max(roundf(cell * 0.1F), 1.0F), line };

                NYA_UIWidgetDraw bar = { .kind = NYA_UI_WIDGET_RULE, .rect = caret, .color = style.accent };
                _nya_ui_draw(ui, &bar);
            }

            // the line's number, right aligned in the gutter, in the dim text colour.
            char number[16];
            (void)snprintf(number, sizeof(number), "%u", i + 1);

            layout->clip = editor_clip;

            NYA_UIWidgetDraw gutter_label = {
                .kind     = NYA_UI_WIDGET_LABEL,
                .rect     = { gutter.x + margin, y, nya_max(gutter_w - (margin * 2.0F), 1.0F), line },
                .label    = number,
                .color    = style.text_dim,
                .as_label = { .room = nya_max(gutter_w - (margin * 2.0F), 1.0F), .overflow = NYA_UI_OVERFLOW_VISIBLE, .align = NYA_UI_ALIGN_END },
            };

            _nya_ui_draw(ui, &gutter_label);
        }

        layout->clip = saved_clip;
    }

    return changed;
}


// INTERNAL

b8 _nya_ui_code_keys(NYA_UI* ui, char* buffer, u32 capacity, u32* length, u32 lines, NYA_UICodeEditor* editor) {
    nya_assert(ui != nullptr && buffer != nullptr && length != nullptr && editor != nullptr);
    nya_assert(*length < capacity);

    const b8* press = _nya_ui.presses;

    NYA_UIText     role      = _nya_ui.layouts[_nya_ui.depth - 1].text;
    NYA_KeyModFlag modifiers = nya_input_modifiers();
    b8             shift     = (modifiers & NYA_KEYMOD_SHIFT) != 0;
    b8             control   = (modifiers & NYA_KEYMOD_CTRL) != 0;

    u32 caret    = nya_min(ui->caret, *length);
    b8  selected = nya_min(ui->select, *length) != caret;
    b8  changed  = false;

    ui->caret = caret;

    // select the whole document.
    if (control && nya_input_key_just_pressed(NYA_KEY_A)) {
        ui->select        = 0;
        ui->caret         = *length;
        editor->_goal_set = false;
        return false;
    }

    // copy, or cut: the selection reaches the clipboard with its newlines intact.
    if (control && (nya_input_key_just_pressed(NYA_KEY_C) || nya_input_key_just_pressed(NYA_KEY_X))) {
        b8 copied = _nya_ui_code_copy(ui, buffer, *length);

        if (copied && nya_input_key_just_pressed(NYA_KEY_X)) {
            *length           = _nya_ui_field_selection_erase(ui, buffer, *length);
            editor->_goal_set = false;
            changed           = true;
        }

        return changed;
    }

    // paste at the caret, replacing the selection. Unlike a one-line field the newlines stay; a stray \r is dropped.
    if (control && nya_input_key_just_pressed(NYA_KEY_V)) {
        if (!nya_clipboard_has_text()) return false;

        NYA_ConstCString pasted = nya_clipboard_text(nya_arena_temp);
        u64              size   = strlen(pasted);
        defer nya_arena_free(nya_arena_temp, (void*)pasted, size + 1);

        if (size == 0) return false;

        char* clean = nya_arena_alloc(nya_arena_temp, size + 1);
        defer nya_arena_free(nya_arena_temp, clean, size + 1);

        u32 kept = 0;
        for (u64 i = 0; i < size; i++) {
            if (pasted[i] != '\r') clean[kept++] = pasted[i];
        }
        clean[kept] = '\0';

        u32 dropped = _nya_ui_field_selection_erase(ui, buffer, *length);
        b8  removed = dropped != *length;

        u32 at    = ui->caret;
        u32 grown = _nya_ui_field_insert(buffer, dropped, capacity, &at, clean);

        _nya_ui_field_caret_set(ui, at, false);

        *length           = grown;
        editor->_goal_set = false;

        return removed || grown != dropped;
    }

    u32 previous = control ? _nya_ui_field_word_start(buffer, caret) : _nya_ui_field_previous(buffer, caret);
    u32 next     = control ? _nya_ui_field_word_end(buffer, *length, caret) : _nya_ui_field_next(buffer, *length, caret);

    // erasing: a selection first, else the character or word to one side; backspace over a line's start takes the newline before it, joining the line to the one above with no special case.
    if (press[_NYA_UI_BACKSPACE]) {
        if (selected) {
            *length = _nya_ui_field_selection_erase(ui, buffer, *length);
            changed = true;
        } else if (caret > 0) {
            *length = _nya_ui_field_erase(buffer, *length, previous, caret);
            _nya_ui_field_caret_set(ui, previous, false);
            changed = true;
        }

        editor->_goal_set = false;
        return changed;
    }

    if (press[_NYA_UI_DELETE]) {
        if (selected) {
            *length = _nya_ui_field_selection_erase(ui, buffer, *length);
            changed = true;
        } else if (next > caret) {
            *length = _nya_ui_field_erase(buffer, *length, caret, next);
            changed = true;
        }

        editor->_goal_set = false;
        return changed;
    }

    // enter splits the line, tab pads it out: both are just inserted text, so the buffer stays a flat run of bytes.
    if (nya_input_key_just_pressed(NYA_KEY_RETURN) || nya_input_key_just_pressed(NYA_KEY_KP_ENTER)) {
        u32 dropped = _nya_ui_field_selection_erase(ui, buffer, *length);
        b8  removed = dropped != *length;

        u32 at    = ui->caret;
        u32 grown = _nya_ui_field_insert(buffer, dropped, capacity, &at, "\n");

        _nya_ui_field_caret_set(ui, at, false);

        *length           = grown;
        editor->_goal_set = false;
        return removed || grown != dropped;
    }

    if (nya_input_key_just_pressed(NYA_KEY_TAB)) {
        u32  width = editor->tab_width == 0 ? NYA_UI_CODE_EDITOR_TAB : nya_min(editor->tab_width, 16U);
        char spaces[17];

        for (u32 i = 0; i < width; i++) spaces[i] = ' ';
        spaces[width] = '\0';

        u32 dropped = _nya_ui_field_selection_erase(ui, buffer, *length);
        b8  removed = dropped != *length;

        u32 at    = ui->caret;
        u32 grown = _nya_ui_field_insert(buffer, dropped, capacity, &at, spaces);

        _nya_ui_field_caret_set(ui, at, false);

        *length           = grown;
        editor->_goal_set = false;
        return removed || grown != dropped;
    }

    // horizontal moves, the same as a field's: a character or, with control, a word; a plain move over a selection collapses to its near edge.
    if (press[_NYA_UI_CARET_LEFT]) {
        _nya_ui_field_caret_set(ui, selected && !shift ? nya_min(ui->select, caret) : previous, shift);
        editor->_goal_set = false;
    } else if (press[_NYA_UI_CARET_RIGHT]) {
        _nya_ui_field_caret_set(ui, selected && !shift ? nya_max(ui->select, caret) : next, shift);
        editor->_goal_set = false;
    }

    // vertical moves keep the column: the x the run started at is aimed for on every line it crosses, so a short line in the middle doesn't drag the caret in; read from the raw arrows, since the press table's menu directions are cleared while a keyboard belongs to an editor.
    b8 up   = nya_input_key_just_pressed(NYA_KEY_UP);
    b8 down = nya_input_key_just_pressed(NYA_KEY_DOWN);

    if (up || down) {
        u32 cur_line, cur_column;
        _nya_ui_code_locate(buffer, ui->caret, &cur_line, &cur_column);

        u32 ls, le;
        _nya_ui_code_line_bounds(buffer, *length, cur_line, &ls, &le);

        if (!editor->_goal_set) {
            editor->_goal_x   = _nya_ui_code_prefix(role, buffer, ls, ui->caret);
            editor->_goal_set = true;
        }

        if (up && cur_line == 0) {
            _nya_ui_field_caret_set(ui, 0, shift);
        } else if (down && cur_line + 1 >= lines) {
            _nya_ui_field_caret_set(ui, *length, shift);
        } else {
            u32 target = up ? cur_line - 1 : cur_line + 1;

            u32 tls, tle;
            _nya_ui_code_line_bounds(buffer, *length, target, &tls, &tle);

            _nya_ui_field_caret_set(ui, _nya_ui_code_offset_at(role, buffer, tls, tle, editor->_goal_x), shift);
        }
    }

    // home and end are the line's ends here, not the buffer's.
    if (nya_input_key_just_pressed(NYA_KEY_HOME) || nya_input_key_just_pressed(NYA_KEY_END)) {
        u32 hl, hc;
        _nya_ui_code_locate(buffer, ui->caret, &hl, &hc);

        u32 ls, le;
        _nya_ui_code_line_bounds(buffer, *length, hl, &ls, &le);

        _nya_ui_field_caret_set(ui, nya_input_key_just_pressed(NYA_KEY_HOME) ? ls : le, shift);
        editor->_goal_set = false;
    }

    nya_assert(ui->caret <= *length && ui->select <= *length, "the caret and selection stay inside the buffer");

    return changed;
}

u32 _nya_ui_code_lines(NYA_ConstCString buffer, u32 length) {
    nya_assert(buffer != nullptr);

    u32 lines = 1;
    for (u32 i = 0; i < length; i++) {
        if (buffer[i] == '\n') lines++;
    }

    return lines;
}

void _nya_ui_code_line_bounds(NYA_ConstCString buffer, u32 length, u32 index, u32* start, u32* end) {
    nya_assert(buffer != nullptr && start != nullptr && end != nullptr);

    u32 line  = 0;
    u32 begin = 0;

    for (u32 i = 0; i < length && line < index; i++) {
        if (buffer[i] == '\n') {
            line++;
            begin = i + 1;
        }
    }

    u32 finish = begin;
    while (finish < length && buffer[finish] != '\n') finish++;

    *start = begin;
    *end   = finish;
}

void _nya_ui_code_locate(NYA_ConstCString buffer, u32 offset, u32* line, u32* column) {
    nya_assert(buffer != nullptr && line != nullptr && column != nullptr);

    u32 rows  = 0;
    u32 begin = 0;

    for (u32 i = 0; i < offset; i++) {
        if (buffer[i] == '\n') {
            rows++;
            begin = i + 1;
        }
    }

    // the column counts codepoints, not bytes, so a multi-byte character is one column wide.
    u32 cols = 0;
    for (u32 i = begin; i < offset; i++) {
        if (((u8)buffer[i] & 0xC0U) != 0x80U) cols++;
    }

    *line   = rows;
    *column = cols;
}

f32 _nya_ui_code_prefix(NYA_UIText role, char* buffer, u32 start, u32 upto) {
    nya_assert(buffer != nullptr && start <= upto);

    if (upto == start) return 0.0F;

    // measured by terminating the run in place, so a line longer than a field's cap is measured whole with no copy; the byte is put straight back, so nothing downstream sees the buffer cut.
    char saved   = buffer[upto];
    buffer[upto] = '\0';

    f32 width = _nya_ui_text_width(role, buffer + start);

    buffer[upto] = saved;

    return width;
}

u32 _nya_ui_code_offset_at(NYA_UIText role, char* buffer, u32 start, u32 end, f32 x) {
    nya_assert(buffer != nullptr && start <= end);

    if (x <= 0.0F || end == start) return start;

    u32 best     = start;
    f32 best_gap = fabsf(x);

    for (u32 offset = _nya_ui_field_next(buffer, end, start); offset <= end; offset = _nya_ui_field_next(buffer, end, offset)) {
        f32 gap = fabsf(x - _nya_ui_code_prefix(role, buffer, start, offset));

        // widths only grow, so the first boundary further from the click than the last one is past it.
        if (gap > best_gap) break;

        best     = offset;
        best_gap = gap;

        if (offset == end) break;
    }

    return best;
}

b8 _nya_ui_code_copy(const NYA_UI* ui, char* buffer, u32 length) {
    nya_assert(ui != nullptr && buffer != nullptr);

    u32 from = nya_min(nya_min(ui->caret, ui->select), length);
    u32 to   = nya_min(nya_max(ui->caret, ui->select), length);

    if (to == from) return false;

    // terminated in place rather than copied to a fixed buffer, so a selection longer than a field's cap still goes.
    char saved = buffer[to];
    buffer[to] = '\0';

    b8 ok = nya_clipboard_text_set(buffer + from).ok;

    buffer[to] = saved;

    return ok;
}
