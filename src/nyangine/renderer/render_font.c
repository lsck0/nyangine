#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    b8               used;
    NYA_ConstCString name;
    NYA_Font         font;
} _NYA_FontEntry;

NYA_INTERNAL _NYA_FontEntry _nya_font_registry[NYA_FONT_REGISTRY_MAX] = { 0 };
NYA_INTERNAL u32            _nya_font_registry_count                  = 0;
NYA_INTERNAL NYA_Font       _nya_font_default                         = { 0 };

/** Compared by string as well as pointer: two call sites naming "ui" hold two different literals. */
NYA_INTERNAL _NYA_FontEntry* _nya_font_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
        _NYA_FontEntry* entry = &_nya_font_registry[i];
        if (!entry->used) continue;
        if (entry->name == name || nya_string_equals(entry->name, name)) return entry;
    }

    return nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Font nya_font(NYA_ConstCString path, f32 point_size) {
    return (NYA_Font){ .path = path, .point_size = point_size };
}

b8 nya_font_valid(NYA_Font font) {
    return font.path != nullptr && font.path[0] != '\0' && font.point_size > 0.0F;
}

b8 nya_font_equals(NYA_Font a, NYA_Font b) {
    if (a.point_size != b.point_size) return false;
    if (a.path == b.path) return true;
    if (a.path == nullptr || b.path == nullptr) return false;

    return nya_string_equals(a.path, b.path);
}

void nya_font_default_set(NYA_Font font) {
    _nya_font_default = font;
}

NYA_Font nya_font_default(void) {
    return _nya_font_default;
}

NYA_Font nya_font_resolve(NYA_Font font) {
    return nya_font_valid(font) ? font : _nya_font_default;
}

b8 nya_font_register(NYA_ConstCString name, NYA_ConstCString path, f32 point_size) {
    // Registered here rather than at some dedicated init: the registry has none, a zeroed static
    // array already being a valid empty one, so this call is the first point the count is
    // meaningful. Guarded so a game (or a test) that registers many fonts over a run does not add
    // a copy of itself to the ceiling registry on every single call.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("fonts", NYA_FONT_REGISTRY_MAX, &_nya_font_registry_count);
        ceiling_registered = true;
    }

    if (name == nullptr || name[0] == '\0') return false;

    NYA_Font font = nya_font(path, point_size);

    // Refused rather than stored: a registry entry that resolves to nothing turns "my text is missing"
    // into a lookup that succeeds, which is the harder failure to trace.
    if (!nya_font_valid(font)) return false;

    _NYA_FontEntry* entry = _nya_font_find(name);

    if (entry == nullptr) {
        for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
            if (_nya_font_registry[i].used) continue;

            entry = &_nya_font_registry[i];
            _nya_font_registry_count++;
            break;
        }
    }

    if (entry == nullptr) {
        nya_log_warn("No free font registry slot for '%s'; " FMTu32 " are in use.", name, (u32)NYA_FONT_REGISTRY_MAX);
        return false;
    }

    *entry = (_NYA_FontEntry){ .used = true, .name = name, .font = font };

    return true;
}

NYA_Font nya_font_named(NYA_ConstCString name) {
    _NYA_FontEntry* entry = _nya_font_find(name);

    return entry != nullptr ? entry->font : NYA_FONT_NONE;
}

b8 nya_font_registered(NYA_ConstCString name) {
    return _nya_font_find(name) != nullptr;
}

void nya_font_unregister(NYA_ConstCString name) {
    _NYA_FontEntry* entry = _nya_font_find(name);
    if (entry == nullptr) return;

    *entry = (_NYA_FontEntry){ 0 };
    _nya_font_registry_count--;
}

void nya_font_clear(void) {
    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) _nya_font_registry[i] = (_NYA_FontEntry){ 0 };

    _nya_font_registry_count = 0;
    _nya_font_default        = NYA_FONT_NONE;
}

u32 nya_font_count(void) {
    return _nya_font_registry_count;
}

NYA_FontMetrics nya_font_metrics(NYA_Font font) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return (NYA_FontMetrics){ 0 };

    /*
     * Read through the current-font state and restored afterwards.
     *
     * render2d exposes line height, ascent and descent for whichever font is current and for no other,
     * so asking about a named one means making it current for the duration. Restoring is what keeps
     * this a *query*: a caller measuring a title font must not silently leave the HUD drawing in it.
     */
    NYA_ConstCString previous_path = nya_render2d_font_get();
    f32              previous_size = nya_render2d_font_size_get();

    nya_render2d_font_set(font.path, font.point_size);

    NYA_FontMetrics metrics = {
        .line_height = nya_render2d_font_line_height(),
        .ascent      = nya_render2d_font_ascent(),
        .descent     = nya_render2d_font_descent(),
        .height      = nya_render2d_font_height(),
    };

    if (previous_path != nullptr) nya_render2d_font_set(previous_path, previous_size);

    return metrics;
}

b8 nya_font_sdf_set(NYA_Font font, b8 enabled) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return false;

    TTF_Font* face = nya_text_font_for(font.path, font.point_size);

    // Not loaded yet, which is normal for the first frames. Refused rather than remembered: a pending
    // mode would have to be reapplied on every reload, and a caller registering fonts at startup can
    // set this again once the face is up.
    if (face == nullptr) return false;

    return TTF_SetFontSDF(face, enabled);
}

b8 nya_font_sdf(NYA_Font font) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return false;

    TTF_Font* face = nya_text_font_for(font.path, font.point_size);

    return face != nullptr && TTF_GetFontSDF(face);
}

f32x2 nya_font_measure(NYA_Font font, NYA_ConstCString text) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font) || text == nullptr) return f32x2_zero;

    return nya_render2d_text_measure_with_font(font.path, font.point_size, text);
}

f32 nya_font_width(NYA_Font font, NYA_ConstCString text) {
    return nya_font_measure(font, text).x;
}

f32 nya_font_height(NYA_Font font, NYA_ConstCString text) {
    return nya_font_measure(font, text).y;
}

void nya_font_draw(NYA_Window* window, NYA_Font font, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font) || text == nullptr) return;

    nya_render2d_text_with_font(window, font.path, font.point_size, text, x, y, color);
}
