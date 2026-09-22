/**
 * @file ui_text.c
 *
 * Typed text: the field behind every editable box, and the single line widget on top of it. See ui.h.
 *
 * The field is one line of UTF-8 in the caller's buffer, with a caret, a selection, word moves and the system
 * clipboard. Offsets are bytes, never codepoints, because that is what the buffer is indexed by; every move lands
 * on a codepoint boundary, so a multi-byte character is never cut in half.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `byte` belongs to a word, for the word moves. Every byte of a multi-byte codepoint does. */
NYA_INTERNAL b8 _nya_ui_field_word_byte(char byte) __attr_no_discard;

/** The codepoint boundary before `offset`, and the one after it. Both stay inside [0, `length`]. */
NYA_INTERNAL u32 _nya_ui_field_previous(NYA_ConstCString text, u32 offset) __attr_no_discard;
NYA_INTERNAL u32 _nya_ui_field_next(NYA_ConstCString text, u32 length, u32 offset) __attr_no_discard;

/** The start of the word at or before `offset`, and the end of the word at or after it. */
NYA_INTERNAL u32 _nya_ui_field_word_start(NYA_ConstCString text, u32 offset) __attr_no_discard;
NYA_INTERNAL u32 _nya_ui_field_word_end(NYA_ConstCString text, u32 length, u32 offset) __attr_no_discard;

/**
 * The codepoint boundary in `text` nearest `x` pixels from its start, at `role`. Quadratic in the buffer, which is
 * bounded by NYA_UI_TEXT_INPUT_MAX and only walked on a click, so it stays a few microseconds and needs no cache.
 * */
NYA_INTERNAL u32 _nya_ui_field_offset_at(NYA_UIText role, NYA_ConstCString text, f32 x) __attr_no_discard;

/** Removes [`from`, `to`) from `buffer`, terminator included. The new length. */
NYA_INTERNAL u32 _nya_ui_field_erase(char* buffer, u32 length, u32 from, u32 to);

/**
 * Inserts as much of `text` at `at` as `capacity` leaves room for, cut on a codepoint boundary. The new length, and
 * `*at` moved past what went in.
 * */
NYA_INTERNAL u32 _nya_ui_field_insert(char* buffer, u32 length, u32 capacity, u32* at, NYA_ConstCString text);

/** Drops the selection, if any, and leaves the caret where it was. The new length. */
NYA_INTERNAL u32 _nya_ui_field_selection_erase(NYA_UI* ui, char* buffer, u32 length);

/** Puts the selection on the system clipboard. False when there is none, or the clipboard refused it. */
NYA_INTERNAL b8 _nya_ui_field_selection_copy(const NYA_UI* ui, NYA_ConstCString buffer);

/** Moves the caret to `offset`, dragging the selection anchor with it unless `keep_selection`. */
NYA_INTERNAL void _nya_ui_field_caret_set(NYA_UI* ui, u32 offset, b8 keep_selection);

/** The pointer, the double click and the drag: where the caret lands and what it selects. True when it moved. */
NYA_INTERNAL b8 _nya_ui_field_pointer(NYA_UI* ui, _NYA_UIWidget widget, NYA_Rectf box, NYA_UIText role, NYA_ConstCString buffer, f32 shift);

/** The keys that edit `buffer`, everything but the plain typed text. True when the text changed. */
NYA_INTERNAL b8 _nya_ui_field_keys(NYA_UI* ui, char* buffer, u32 capacity, u32* length);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_text_input(NYA_UI* ui, NYA_ConstCString label, char* buffer, u32 capacity) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && buffer != nullptr);
    nya_assert(capacity > 0 && capacity <= NYA_UI_TEXT_INPUT_MAX, "a field edits 1 to NYA_UI_TEXT_INPUT_MAX bytes, got %u", capacity);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 line   = look->line_heights[layout->text];
    f32 height = _nya_ui_item_height(layout);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (height * 3.0F), height }, true);

    // the right part of the row past the label, like a slider's track.
    f32       field_x = roundf(rect.x + nya_max(rect.width * 0.4F, text_width + (look->padding * 2.0F)));
    f32       inset   = roundf(nya_min(look->padding * 0.5F, (height - line) * 0.5F));
    NYA_Rectf box     = { field_x, rect.y + inset, nya_max(rect.x + rect.width - look->padding - field_x, 1.0F), height - (inset * 2.0F) };

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    NYA_UIFieldDraw field   = { 0 };
    b8              changed = _nya_ui_field(ui, widget, widget.activated, rect, box, buffer, capacity, &field);

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind     = NYA_UI_WIDGET_FIELD,
            .rect     = rect,
            .state    = _nya_ui_state(ui, widget),
            .label    = label,
            .as_field = { .field = field },
        };

        _nya_ui_draw(ui, &draw);
    }

    return changed;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_ui_field(NYA_UI* ui, _NYA_UIWidget widget, b8 start, NYA_Rectf owner, NYA_Rectf box, char* buffer, u32 capacity, NYA_UIFieldDraw* out) {
    nya_assert(ui != nullptr && buffer != nullptr && out != nullptr);
    nya_assert(capacity > 0 && capacity <= NYA_UI_TEXT_INPUT_MAX);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    NYA_UIText role = layout->text;

    // a buffer the caller filled without a terminator in range reads as full.
    buffer[capacity - 1] = '\0';

    u32 length  = (u32)strlen(buffer);
    b8  changed = false;
    b8  editing = ui->editing == widget.id;

    // where the text starts inside the box, and how far it is scrolled so the caret stays visible.
    f32 margin = roundf(look->padding * 0.5F);
    f32 room   = nya_max(box.width - (margin * 2.0F), 1.0F);
    f32 shift  = editing ? nya_max(_nya_ui_measure_bytes(role, buffer, nya_min(ui->caret, length)) - room, 0.0F) : 0.0F;

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

            // a character that did not fit changed nothing, and must not report that it did.
            changed = removed || grown != dropped;
            length  = grown;
        }

        changed |= _nya_ui_field_keys(ui, buffer, capacity, &length);
        changed |= _nya_ui_field_pointer(ui, widget, box, role, buffer, shift);

        b8 returned = nya_input_key_just_pressed(NYA_KEY_RETURN) || nya_input_key_just_pressed(NYA_KEY_KP_ENTER);

        // a press on a panel over this one landed elsewhere however far inside the field's own rectangle it was.
        b8 outside = _nya_ui.pointer_pressed && (layout->covered || !nya_rect_contains(owner, _nya_ui.pointer));

        // a confirm that typed something is the space bar.
        if (returned || (_nya_ui.confirm && size == 0) || _nya_ui.cancel || outside) {
            _nya_ui_typing_stop(ui);
            _nya_ui.cancel = false;
            editing        = false;
        }

        // the caret may have moved far enough to scroll the line under it.
        shift = editing ? nya_max(_nya_ui_measure_bytes(role, buffer, nya_min(ui->caret, length)) - room, 0.0F) : 0.0F;
    } else if (ui->pass == NYA_UI_PASS_INPUT && start && !widget.disabled) {
        // opened by a click: the caret lands where the click did, opened by confirm: at the end, ready to append.
        u32 caret = length;

        if (_nya_ui.pointer_released && nya_rect_contains(box, _nya_ui.pointer)) {
            caret = _nya_ui_field_offset_at(role, buffer, _nya_ui.pointer.x - (box.x + margin));
        }

        _nya_ui_typing_start(ui, widget.id, caret, box);
    }

    // the IME's run is clamped each end on its own, since the platform's numbers are not ours to trust with a sum.
    NYA_ConstCString composing = editing ? nya_input_text_composition() : "";
    u32              composed  = (u32)strlen(composing);
    u32              from      = 0;
    u32              to        = 0;

    if (composed > 0) {
        s32 first = 0, selected = 0;
        nya_input_text_composition_range(&first, &selected);

        from = (u32)nya_clamp(first, 0, (s32)composed);
        to   = from + (u32)nya_clamp(selected, 0, (s32)(composed - from));
    }

    *out = (NYA_UIFieldDraw){
        .box            = box,
        .buffer         = buffer,
        .composing      = composing,
        .caret          = ui->caret,
        .select         = ui->select,
        .composing_from = from,
        .composing_to   = to,
        .shift          = shift,
        .editing        = editing,
    };

    return changed;
}

b8 _nya_ui_field_word_byte(char byte) {
    u8 value = (u8)byte;

    // every continuation and lead byte counts, so a word of non-ASCII text moves as one.
    return value >= 0x80U || isalnum(value) != 0 || byte == '_';
}

u32 _nya_ui_field_previous(NYA_ConstCString text, u32 offset) {
    nya_assert(text != nullptr);

    if (offset == 0) return 0;

    u32 previous = offset - 1;
    while (previous > 0 && ((u8)text[previous] & 0xC0U) == 0x80U) previous--;

    nya_assert(previous < offset);

    return previous;
}

u32 _nya_ui_field_next(NYA_ConstCString text, u32 length, u32 offset) {
    nya_assert(text != nullptr);

    if (offset >= length) return length;

    return offset + nya_min(nya_max(nya_utf8_length(text + offset), 1U), length - offset);
}

u32 _nya_ui_field_word_start(NYA_ConstCString text, u32 offset) {
    nya_assert(text != nullptr);

    u32 at = offset;

    while (at > 0 && !_nya_ui_field_word_byte(text[_nya_ui_field_previous(text, at)])) at = _nya_ui_field_previous(text, at);
    while (at > 0 && _nya_ui_field_word_byte(text[_nya_ui_field_previous(text, at)])) at = _nya_ui_field_previous(text, at);

    nya_assert(at <= offset);

    return at;
}

u32 _nya_ui_field_word_end(NYA_ConstCString text, u32 length, u32 offset) {
    nya_assert(text != nullptr && offset <= length);

    u32 at = offset;

    while (at < length && !_nya_ui_field_word_byte(text[at])) at = _nya_ui_field_next(text, length, at);
    while (at < length && _nya_ui_field_word_byte(text[at])) at = _nya_ui_field_next(text, length, at);

    nya_assert(at >= offset && at <= length);

    return at;
}

u32 _nya_ui_field_offset_at(NYA_UIText role, NYA_ConstCString text, f32 x) {
    nya_assert(text != nullptr);

    u32 length = (u32)strlen(text);
    if (x <= 0.0F || length == 0) return 0;

    u32 best     = 0;
    f32 best_gap = fabsf(x);

    for (u32 offset = _nya_ui_field_next(text, length, 0); offset <= length; offset = _nya_ui_field_next(text, length, offset)) {
        f32 gap = fabsf(x - _nya_ui_measure_bytes(role, text, offset));

        // widths only grow, so the first boundary that is further from the click than the last one is past it.
        if (gap > best_gap) break;

        best     = offset;
        best_gap = gap;

        if (offset == length) break;
    }

    nya_assert(best <= length);

    return best;
}

u32 _nya_ui_field_erase(char* buffer, u32 length, u32 from, u32 to) {
    nya_assert(buffer != nullptr);
    nya_assert(from <= to && to <= length, "erasing [%u, %u) of %u bytes", from, to, length);

    if (to == from) return length;

    nya_memmove(buffer + from, buffer + to, length - to + 1);

    return length - (to - from);
}

u32 _nya_ui_field_insert(char* buffer, u32 length, u32 capacity, u32* at, NYA_ConstCString text) {
    nya_assert(buffer != nullptr && at != nullptr && text != nullptr);
    nya_assert(*at <= length && length < capacity);

    u32 size = (u32)strlen(text);
    u32 fits = nya_min(size, capacity - 1 - length);

    // cut on a codepoint boundary when the buffer fills, so a half character never lands in it.
    while (fits > 0 && fits < size && ((u8)text[fits] & 0xC0U) == 0x80U) fits--;

    if (fits == 0) return length;

    nya_memmove(buffer + *at + fits, buffer + *at, length - *at + 1);
    nya_memcpy(buffer + *at, text, fits);

    *at += fits;

    nya_assert(length + fits < capacity);

    return length + fits;
}

u32 _nya_ui_field_selection_erase(NYA_UI* ui, char* buffer, u32 length) {
    nya_assert(ui != nullptr && buffer != nullptr);

    u32 from = nya_min(nya_min(ui->caret, ui->select), length);
    u32 to   = nya_min(nya_max(ui->caret, ui->select), length);

    if (to == from) return length;

    _nya_ui_field_caret_set(ui, from, false);

    return _nya_ui_field_erase(buffer, length, from, to);
}

b8 _nya_ui_field_selection_copy(const NYA_UI* ui, NYA_ConstCString buffer) {
    nya_assert(ui != nullptr && buffer != nullptr);

    u32 length = (u32)strlen(buffer);
    u32 from   = nya_min(nya_min(ui->caret, ui->select), length);
    u32 to     = nya_min(nya_max(ui->caret, ui->select), length);

    if (to == from) return false;

    char selected[NYA_UI_TEXT_INPUT_MAX];
    u32  size = nya_min(to - from, (u32)sizeof(selected) - 1);

    nya_memcpy(selected, buffer + from, size);
    selected[size] = '\0';

    return nya_clipboard_text_set(selected).ok;
}

void _nya_ui_field_caret_set(NYA_UI* ui, u32 offset, b8 keep_selection) {
    nya_assert(ui != nullptr);
    nya_assert(ui->editing != 0, "only the field with the keyboard has a caret");

    ui->caret = offset;
    if (!keep_selection) ui->select = offset;
}

b8 _nya_ui_field_pointer(NYA_UI* ui, _NYA_UIWidget widget, NYA_Rectf box, NYA_UIText role, NYA_ConstCString buffer, f32 shift) {
    nya_assert(ui != nullptr && buffer != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    f32 margin = roundf(_nya_ui_look()->padding * 0.5F);
    f32 local  = _nya_ui.pointer.x - (box.x + margin) + shift;
    u32 length = (u32)strlen(buffer);

    // the drag below stays ungated: it belongs to the press that already won the pointer, wherever it travels.
    if (_nya_ui.pointer_pressed && !layout->covered && nya_rect_contains(box, _nya_ui.pointer)) {
        f64 now    = nya_app_uptime_s();
        u32 offset = _nya_ui_field_offset_at(role, buffer, local);

        // a second click on the same field, soon enough and on the same word, takes the whole word.
        b8 again = ui->click_id == widget.id && now - ui->click_s < NYA_UI_DOUBLE_CLICK_S;

        ui->click_id = widget.id;
        ui->click_s  = now;

        if (again) {
            ui->select = _nya_ui_field_word_start(buffer, offset);
            ui->caret  = _nya_ui_field_word_end(buffer, length, offset);
        } else {
            _nya_ui_field_caret_set(ui, offset, (nya_input_modifiers() & NYA_KEYMOD_SHIFT) != 0);
        }

        ui->dragging = true;
        return false;
    }

    // held after a press in the box: the caret follows the pointer and the anchor stays where the press landed.
    if (ui->active == widget.id && ui->dragging && _nya_ui.pointer_down && _nya_ui.pointer_moved) {
        _nya_ui_field_caret_set(ui, _nya_ui_field_offset_at(role, buffer, local), true);
    }

    return false;
}

b8 _nya_ui_field_keys(NYA_UI* ui, char* buffer, u32 capacity, u32* length) {
    nya_assert(ui != nullptr && buffer != nullptr && length != nullptr);
    nya_assert(*length < capacity);

    const b8* press = _nya_ui.presses;

    NYA_KeyModFlag modifiers = nya_input_modifiers();
    b8             shift     = (modifiers & NYA_KEYMOD_SHIFT) != 0;
    b8             control   = (modifiers & NYA_KEYMOD_CTRL) != 0;

    u32 caret    = nya_min(ui->caret, *length);
    b8  selected = nya_min(ui->select, *length) != caret;
    b8  changed  = false;

    ui->caret = caret;

    if (control && nya_input_key_just_pressed(NYA_KEY_A)) {
        ui->select = 0;
        ui->caret  = *length;

        return false;
    }

    if (control && (nya_input_key_just_pressed(NYA_KEY_C) || nya_input_key_just_pressed(NYA_KEY_X))) {
        b8 copied = _nya_ui_field_selection_copy(ui, buffer);

        if (copied && nya_input_key_just_pressed(NYA_KEY_X)) {
            *length = _nya_ui_field_selection_erase(ui, buffer, *length);
            changed = true;
        }

        return changed;
    }

    if (control && nya_input_key_just_pressed(NYA_KEY_V)) {
        // asked first, since fetching copies whatever is there, and an image or nothing at all is common.
        if (!nya_clipboard_has_text()) return false;

        NYA_ConstCString pasted = nya_clipboard_text(nya_arena_temp);
        u64              size   = strlen(pasted);
        defer nya_arena_free(nya_arena_temp, (void*)pasted, size + 1);

        if (size == 0) return false;

        u32 dropped = _nya_ui_field_selection_erase(ui, buffer, *length);
        b8  removed = dropped != *length;

        // one line: a pasted newline would otherwise sit in the buffer as an unrenderable glyph.
        char flat[NYA_UI_TEXT_INPUT_MAX];
        u32  flat_size = nya_min((u32)size, (u32)sizeof(flat) - 1);

        for (u32 i = 0; i < flat_size; i++) flat[i] = pasted[i] == '\n' || pasted[i] == '\r' || pasted[i] == '\t' ? ' ' : pasted[i];
        flat[flat_size] = '\0';

        u32 at    = ui->caret;
        u32 grown = _nya_ui_field_insert(buffer, dropped, capacity, &at, flat);

        _nya_ui_field_caret_set(ui, at, false);

        *length = grown;

        return removed || grown != dropped;
    }

    u32 previous = control ? _nya_ui_field_word_start(buffer, caret) : _nya_ui_field_previous(buffer, caret);
    u32 next     = control ? _nya_ui_field_word_end(buffer, *length, caret) : _nya_ui_field_next(buffer, *length, caret);

    if (press[_NYA_UI_BACKSPACE]) {
        // backspace over a selection takes the selection, not the character before it.
        if (selected) {
            *length = _nya_ui_field_selection_erase(ui, buffer, *length);
            changed = true;
        } else if (caret > 0) {
            *length = _nya_ui_field_erase(buffer, *length, previous, caret);
            _nya_ui_field_caret_set(ui, previous, false);
            changed = true;
        }
    } else if (press[_NYA_UI_DELETE]) {
        if (selected) {
            *length = _nya_ui_field_selection_erase(ui, buffer, *length);
            changed = true;
        } else if (next > caret) {
            *length = _nya_ui_field_erase(buffer, *length, caret, next);
            changed = true;
        }
    } else if (press[_NYA_UI_CARET_LEFT]) {
        // an unshifted move over a selection collapses to its near edge instead of stepping from the caret.
        _nya_ui_field_caret_set(ui, selected && !shift ? nya_min(ui->select, caret) : previous, shift);
    } else if (press[_NYA_UI_CARET_RIGHT]) {
        _nya_ui_field_caret_set(ui, selected && !shift ? nya_max(ui->select, caret) : next, shift);
    }

    if (nya_input_key_just_pressed(NYA_KEY_HOME)) _nya_ui_field_caret_set(ui, 0, shift);
    if (nya_input_key_just_pressed(NYA_KEY_END)) _nya_ui_field_caret_set(ui, *length, shift);

    nya_assert(ui->caret <= *length && ui->select <= *length, "the caret and selection stay inside the buffer");

    return changed;
}
