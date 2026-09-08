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
 *
 * Kept because the request almost always arrives before there is anything to apply it to. A face is
 * loaded by the asset system over the frames *after* it is first named, and fonts are registered at
 * startup, so `nya_font_sdf_set` at the natural call site had no `TTF_Font` and used to answer false
 * and forget — which made the mode reachable only by a caller willing to poll for the face.
 *
 * Keyed by path and size rather than by registry name: `nya_font_sdf_set` takes an NYA_Font, which is
 * that pair and not a name, and a font never registered under any name is still a legitimate thing to
 * ask about.
 * */
typedef struct {
    b8               used;
    NYA_ConstCString path;
    f32              point_size;

    /** What was asked for. */
    b8 sdf;

    /**
     * The face this was last pushed onto, or null while it has never been pushed.
     *
     * A pointer rather than a flag, so a *reload* re-applies: hot reload replaces the asset's TTF_Font
     * with a new one built from the new file, and a mode set on the old face does not come with it.
     * The same comparison NYA_FontAtlas.source_font makes, for the same reason.
     * */
    TTF_Font* applied_to;
} _NYA_FontSdfRequest;

/** One per registry slot: a game cannot ask for more distinct fonts than it can register. */
NYA_INTERNAL _NYA_FontSdfRequest _nya_font_sdf_requests[NYA_FONT_REGISTRY_MAX] = { 0 };

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
 *
 * Called from each entry point that is about to reach a face, which is what makes the deferral
 * invisible to a caller. It has to run *before* the draw or the measurement it precedes, and that
 * ordering is the whole point: the mode changes the face's metrics and what its glyphs rasterise to,
 * and render2d bakes an atlas — sized from those metrics — the first time a glyph is drawn from it.
 * Applying afterwards would leave an atlas full of coverage bitmaps flagged as a distance field.
 *
 * Cheap: NYA_FONT_REGISTRY_MAX is 32, almost every slot is unused, and a slot already applied to the
 * face it resolves to does nothing at all.
 * */
void _nya_font_sdf_apply_pending(void);

/**
 * Applies outstanding requests at the end of every frame, so one lands the moment its face resolves.
 *
 * The entry points below apply them too, and that alone is not enough: it makes the mode depend on
 * *who reaches the face first*. A face resolved by the immediate-mode text API, or by anything else
 * holding a path and a size, would be baked into an atlas with no mode on it — and the atlas latches
 * what it was baked from, so nothing later would put it right. A frame hook makes the request land
 * against the clock instead of against a call order nobody controls.
 *
 * After the asset system's own frame-ended pass, which is what registration order gets us: the asset
 * system is brought up long before any font is registered, so its hook runs first and a face queued
 * this frame is already resolved by the time this runs.
 *
 * Not static, for the same reason `_nya_config_watch_tick` is not: a registered callback is
 * re-resolved by name through dlsym after a code hot reload, and a symbol with internal linkage
 * cannot be found again.
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
    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) {
        _NYA_FontSdfRequest* request = &_nya_font_sdf_requests[i];

        if (!request->used) continue;

        TTF_Font* face = nya_text_font_for(request->path, request->point_size);

        // Still queued. Normal for the first frames after a font is named; tried again next call.
        if (face == nullptr) continue;

        if (face == request->applied_to) continue;

        // Recorded even when the renderer refuses, so a face that cannot do it is asked once rather
        // than on every draw for the rest of the run.
        if (!TTF_SetFontSDF(face, request->sdf)) {
            nya_log_warn("The renderer refused a distance field for '%s' at %.0f: %s", request->path, (f64)request->point_size,
                         SDL_GetError());
        }

        request->applied_to = face;
    }
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

    // The distance-field requests go with them: they are keyed by path and size rather than by name,
    // so nothing else would ever drop one, and a request outliving the registry would silently reapply
    // itself to the next font that happened to share a path and a size.
    for (u32 i = 0; i < NYA_FONT_REGISTRY_MAX; i++) _nya_font_sdf_requests[i] = (_NYA_FontSdfRequest){ 0 };

    _nya_font_registry_count = 0;
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

        // The path is held, not copied — the same assumption the name registry makes of its names, and
        // true of the generated asset handles and string literals every caller actually passes.
        *request = (_NYA_FontSdfRequest){ .used = true, .path = font.path, .point_size = font.point_size };

        _nya_font_sdf_hook_register();
    }

    // A changed answer has to be pushed again, even onto the face it was already pushed onto.
    if (request->sdf != enabled) request->applied_to = nullptr;

    request->sdf = enabled;

    // Applied now if there is a face, and by whichever entry point reaches one first if there is not.
    _nya_font_sdf_apply_pending();

    return true;
}

b8 nya_font_sdf(NYA_Font font) {
    font = nya_font_resolve(font);
    if (!nya_font_valid(font)) return false;

    _nya_font_sdf_apply_pending();

    TTF_Font* face = nya_text_font_for(font.path, font.point_size);

    // The face is the authority once there is one: it is what the atlas will be baked from.
    if (face != nullptr) return TTF_GetFontSDF(face);

    // No face yet, so report what it is going to be. Answering false here would mean a caller that has
    // just asked for a distance field is told it did not get one, which was the old behaviour and is
    // indistinguishable from the request having been dropped.
    _NYA_FontSdfRequest* request = _nya_font_sdf_find(font);

    return request != nullptr && request->sdf;
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
