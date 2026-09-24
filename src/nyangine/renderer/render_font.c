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

/**
 * A distance-field mode somebody asked for, and the face it was last pushed onto.
 * */
typedef struct {
    b8               used;
    NYA_ConstCString path;
    f32              point_size;

    /** What was asked for. */
    b8 sdf;

    /**
     * The face this was last pushed onto, or null while it has never been pushed.
     * */
    TTF_Font* applied_to;
} _NYA_FontSdfRequest;

/** One per registry slot: a game cannot ask for more distinct fonts than it can register. */
NYA_INTERNAL _NYA_FontSdfRequest _nya_font_sdf_requests[NYA_FONT_REGISTRY_MAX] = { 0 };

/**
 * The asset generation at which every request last sat on its face. A face only appears or changes by loading, so
 * until the generation moves a measure or draw has nothing to push. U64_MAX while a request waits.
 * */
NYA_INTERNAL u64 _nya_font_sdf_settled_generation = U64_MAX;

/** Compared by string as well as pointer, for the reason _nya_font_find is. */
NYA_INTERNAL _NYA_FontSdfRequest* _nya_font_sdf_find(NYA_Font font) {
    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
        _NYA_FontSdfRequest* request = &_nya_font_sdf_requests[i];

        if (!request->used) continue;
        if (request->point_size != font.point_size) continue;
        if (request->path == font.path || nya_string_equals(request->path, font.path)) return request;
    }

    return nullptr;
}

/**
 * Pushes every outstanding request onto its face, for the ones whose faces exist yet.
 * */
NYA_INTERNAL void _nya_font_sdf_apply_pending(void);

/**
 * Applies outstanding requests at the end of every frame, so one lands the moment its face resolves.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_font_sdf_tick(NYA_Event* event) {
    nya_unused(event);

    _nya_font_sdf_apply_pending();
}

/** Registers the frame hook once, on the first request. The registry has no init to put it in. */
NYA_INTERNAL void _nya_font_sdf_hook_register(void) {
    static b8 registered = false;
    if (registered) return;

    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_font_sdf_tick),
    });

    registered = true;
}

void _nya_font_sdf_apply_pending(void) {
    u64 generation = nya_asset_generation();

    if (generation == _nya_font_sdf_settled_generation) return;

    b8 settled = true;

    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
        _NYA_FontSdfRequest* request = &_nya_font_sdf_requests[i];

        if (!request->used) continue;

        TTF_Font* face = nya_text_font_for(request->path, request->point_size);

        // Still queued. Normal for the first frames after a font is named; tried again next call.
        if (face == nullptr) {
            settled = false;
            continue;
        }

        if (face == request->applied_to) continue;

        // Recorded even when the renderer refuses, so a face that cannot do it is asked once, not every draw.
        if (!TTF_SetFontSDF(face, request->sdf)) {
            nya_log_warn("The renderer refused a distance field for '%s' at %.0f: %s", request->path, (f64)request->point_size,
                         SDL_GetError());
        }

        request->applied_to = face;
    }

    _nya_font_sdf_settled_generation = settled ? generation : U64_MAX;
}

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
    // Registered on first use since the registry has no init; guarded so many fonts register the ceiling once.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("fonts", NYA_FONT_REGISTRY_MAX, &_nya_font_registry_count);
        ceiling_registered = true;
    }

    if (name == nullptr || name[0] == '\0') return false;

    NYA_Font font = nya_font(path, point_size);

    // Refused rather than stored: an entry resolving to nothing turns missing text into a succeeding lookup, harder to trace.
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

    // The distance-field requests go too: keyed by path and size, one outliving the registry would silently reapply to a font sharing them.
    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) _nya_font_sdf_requests[i] = (_NYA_FontSdfRequest){ 0 };

    _nya_font_sdf_settled_generation = U64_MAX;
    _nya_font_registry_count         = 0;
    _nya_font_default        = NYA_FONT_NONE;
}

u32 nya_font_count(void) {
    return _nya_font_registry_count;
}

NYA_FontMetrics nya_font_metrics(NYA_Font font) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return (NYA_FontMetrics){ 0 };

    // Before the face is reached, never after: see _nya_font_sdf_apply_pending.
    _nya_font_sdf_apply_pending();

    // Read through the current-font state and restored afterwards.
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

    _NYA_FontSdfRequest* request = _nya_font_sdf_find(font);

    if (request == nullptr) {
        for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
            if (_nya_font_sdf_requests[i].used) continue;

            request = &_nya_font_sdf_requests[i];
            break;
        }

        if (request == nullptr) {
            nya_log_warn("No free distance-field request slot for '%s'; " FMTu32 " are in use.", font.path, (u32)NYA_FONT_REGISTRY_MAX);
            return false;
        }

        // the path is held, not copied, like registry names. Callers pass asset handles and string literals.
        *request = (_NYA_FontSdfRequest){ .used = true, .path = font.path, .point_size = font.point_size };

        _nya_font_sdf_hook_register();
    }

    // A changed answer has to be pushed again, even onto the face it was already pushed onto.
    if (request->sdf != enabled) request->applied_to = nullptr;

    request->sdf                     = enabled;
    _nya_font_sdf_settled_generation = U64_MAX;

    // Applied now if there is a face, and by whichever entry point reaches one first if there is not.
    _nya_font_sdf_apply_pending();

    return true;
}

b8 nya_font_sdf(NYA_Font font) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return false;

    _NYA_FontSdfRequest* request = _nya_font_sdf_find(font);

    // Asked before any face is reached: with no request the answer is false, and reaching for the face would queue a load nobody asked for (the terminal backend has no loader anyway).
    if (request == nullptr) return false;

    _nya_font_sdf_apply_pending();

    TTF_Font* face = nya_text_font_for(font.path, font.point_size);

    // The face is the authority once there is one: it is what the atlas will be baked from.
    if (face != nullptr) return TTF_GetFontSDF(face);

    // No face yet, so report what it is going to be; answering false would look like the request was dropped.
    return request->sdf;
}

f32x2 nya_font_measure(NYA_Font font, NYA_ConstCString text) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font) || text == nullptr) return f32x2_zero;

    // Before the face is reached, never after: see _nya_font_sdf_apply_pending.
    _nya_font_sdf_apply_pending();

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

    // Before the face is reached, never after: see _nya_font_sdf_apply_pending.
    _nya_font_sdf_apply_pending();

    nya_render2d_text_with_font(window, font.path, font.point_size, text, x, y, color);
}

f32x2 nya_font_measure_wrapped(NYA_Font font, NYA_ConstCString text, f32 width) {
    nya_assert(width >= 0.0F);

    font = nya_font_resolve(font);
    if (!nya_font_valid(font) || text == nullptr) return f32x2_zero;

    _nya_font_sdf_apply_pending();

    return nya_render2d_text_box_measure(text, (NYA_Render2DTextBox){ .width = width, .font_path = font.path, .point_size = font.point_size });
}

void nya_font_draw_wrapped(NYA_Window* window, NYA_Font font, NYA_ConstCString text, f32 x, f32 y, f32 width, NYA_TextAlign align, NYA_Color color) {
    nya_assert(width >= 0.0F && align < NYA_TEXT_ALIGN_COUNT);

    font = nya_font_resolve(font);
    if (!nya_font_valid(font) || text == nullptr) return;

    _nya_font_sdf_apply_pending();

    (void)nya_render2d_text_box(window, text, (NYA_Render2DTextBox){ .x = x, .y = y, .width = width, .align = align, .color = color, .font_path = font.path, .point_size = font.point_size });
}
