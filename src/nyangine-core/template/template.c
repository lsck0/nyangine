#include "nyangine-core/nyangine.h"

/*
 * ───────────────────────────────────── OVERVIEW ─────────────────────────────────────
 *
 * The renderer is a single left-to-right pass over the template. Literal text between constructs is
 * copied out verbatim — it is authored, so it is trusted. The three constructs are `{{ }}`
 * interpolation, `{% %}` control tags and `{# #}` comments; anything else is literal.
 *
 * Blocks (`if`/`for`) nest, so the pass is recursive: `_render_block` renders until it meets a tag
 * that closes the block it was asked to render (`endif`, `endfor`, or an `else` that splits one), and
 * hands that reason back to its caller. The caller — `_render_if` or `_render_for` — decides what to
 * do with it. A `for` body is simply re-rendered from the same start position once per element.
 *
 * The `emit` flag threads through the whole recursion. When it is false — the untaken arm of an `if`,
 * the body of a `for` over an empty list — the pass still walks the structure so unbalanced tags are
 * caught, but writes nothing. This is how one code path both renders and skips.
 *
 * Every write goes through `_put`, which is where the output cap lives; loop iterations and block
 * depth are capped at their own gates. Hitting any cap fails the whole render rather than truncating.
 */

// ───────────────────────────────────── STATE ─────────────────────────────────────

/** One loop variable in scope: the name it binds, the element it points at, and its position. */
typedef struct {
    NYA_ConstCString name;
    const NYA_Value* value;
    s64              index0;
    s64              length;
} _NyaTemplateScope;

/** Everything one render carries. Lives on the stack of nya_template_render. */
typedef struct {
    NYA_Arena*         arena;
    const NYA_Object*  context;
    NYA_TemplateEscape mode;
    NYA_String*        out;

    _NyaTemplateScope scope[NYA_TEMPLATE_MAX_DEPTH];
    u32               scope_count;

    u64 iterations;
} _NyaTemplate;

/** Why `_render_block` stopped. `EOF` is the top level running out of template. */
typedef enum {
    _NYA_TEMPLATE_STOP_EOF,
    _NYA_TEMPLATE_STOP_ENDIF,
    _NYA_TEMPLATE_STOP_ENDFOR,
    _NYA_TEMPLATE_STOP_ELSE,
} _NyaTemplateStop;

// ───────────────────────────────────── PRIVATE API ─────────────────────────────────────

NYA_INTERNAL NYA_Error _nya_template_render_block(_NyaTemplate* t, b8 emit, u32 depth, const char** cursor, OUT _NyaTemplateStop* stop);

// ───────────────────────────────────── TEXT HELPERS ─────────────────────────────────────

/** A slice `[ptr, ptr + length)` of the template text, since none of it is null terminated in place. */
typedef struct {
    const char* ptr;
    u64         length;
} _NyaTemplateSlice;

NYA_INTERNAL b8 _nya_template_slice_eq(_NyaTemplateSlice s, NYA_ConstCString literal) {
    u64 n = strlen(literal);
    return s.length == n && (n == 0 || memcmp(s.ptr, literal, n) == 0);
}

/** Drops ASCII spaces and tabs from both ends of a slice. */
NYA_INTERNAL _NyaTemplateSlice _nya_template_trim(_NyaTemplateSlice s) {
    while (s.length > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t' || s.ptr[0] == '\n' || s.ptr[0] == '\r')) {
        s.ptr++;
        s.length--;
    }
    while (s.length > 0) {
        char c = s.ptr[s.length - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        s.length--;
    }
    return s;
}

/** Splits off the leading run of non-whitespace as `head` and returns the trimmed remainder as `rest`. */
NYA_INTERNAL _NyaTemplateSlice _nya_template_word(_NyaTemplateSlice s, OUT _NyaTemplateSlice* rest) {
    s = _nya_template_trim(s);
    u64 i = 0;
    while (i < s.length && s.ptr[i] != ' ' && s.ptr[i] != '\t' && s.ptr[i] != '\n' && s.ptr[i] != '\r') i++;

    _NyaTemplateSlice head = { .ptr = s.ptr, .length = i };
    *rest                  = _nya_template_trim((_NyaTemplateSlice){ .ptr = s.ptr + i, .length = s.length - i });
    return head;
}

/** Finds `needle` in `[from, end)`, or null. `needle` is two characters. */
NYA_INTERNAL const char* _nya_template_find2(const char* from, const char* end, char a, char b) {
    for (const char* p = from; p + 1 < end; p++) {
        if (p[0] == a && p[1] == b) return p;
    }
    return nullptr;
}

// ───────────────────────────────────── OUTPUT ─────────────────────────────────────

/** The one place output grows, so the one place the size cap is enforced. */
NYA_INTERNAL NYA_Error _nya_template_put(_NyaTemplate* t, b8 emit, const u8* data, u64 length) {
    if (!emit || length == 0) return NYA_OK;
    if (t->out->length + length > NYA_TEMPLATE_MAX_OUTPUT) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "template output would exceed %u bytes", NYA_TEMPLATE_MAX_OUTPUT);
    }
    for (u64 i = 0; i < length; i++) nya_string_push_back(t->out, data[i]);
    return NYA_OK;
}

NYA_INTERNAL NYA_Error _nya_template_put_cstr(_NyaTemplate* t, b8 emit, NYA_ConstCString s) {
    return _nya_template_put(t, emit, (const u8*)s, strlen(s));
}

/**
 * Appends `data` with every metacharacter of `mode` neutralised. This is the whole safety story for
 * an interpolated value: a value carrying `<script>` or `\command` or `$x$` comes out inert.
 * */
NYA_INTERNAL NYA_Error _nya_template_put_escaped(_NyaTemplate* t, b8 emit, const u8* data, u64 length, NYA_TemplateEscape mode) {
    if (!emit) return NYA_OK;

    for (u64 i = 0; i < length; i++) {
        u8              c           = data[i];
        NYA_ConstCString replacement = nullptr;

        switch (mode) {
            case NYA_TEMPLATE_ESCAPE_HTML:
                // The same five the UI's HTML presenter encodes; see _nya_ui_html_escape.
                switch (c) {
                    case '&':  replacement = "&amp;"; break;
                    case '<':  replacement = "&lt;"; break;
                    case '>':  replacement = "&gt;"; break;
                    case '"':  replacement = "&quot;"; break;
                    case '\'': replacement = "&#39;"; break;
                    default:   break;
                }
                break;

            case NYA_TEMPLATE_ESCAPE_LATEX:
                // The ten characters LaTeX gives a meaning; the backslash and two accents map to their text-mode command names, since a backslash escape would be a live command.
                switch (c) {
                    case '\\': replacement = "\\textbackslash{}"; break;
                    case '&':  replacement = "\\&"; break;
                    case '%':  replacement = "\\%"; break;
                    case '$':  replacement = "\\$"; break;
                    case '#':  replacement = "\\#"; break;
                    case '_':  replacement = "\\_"; break;
                    case '{':  replacement = "\\{"; break;
                    case '}':  replacement = "\\}"; break;
                    case '~':  replacement = "\\textasciitilde{}"; break;
                    case '^':  replacement = "\\textasciicircum{}"; break;
                    default:   break;
                }
                break;

            case NYA_TEMPLATE_ESCAPE_NONE:
            default: break;
        }

        if (replacement != nullptr) {
            NYA_TRY(_nya_template_put_cstr(t, emit, replacement));
        } else {
            NYA_TRY(_nya_template_put(t, emit, &c, 1));
        }
    }
    return NYA_OK;
}

// ───────────────────────────────────── VALUES ─────────────────────────────────────

/** Renders one scalar value to text, mirroring serde's type mapping. Objects and arrays render empty. */
NYA_INTERNAL NYA_String* _nya_template_value_to_string(NYA_Arena* arena, const NYA_Value* v) {
    NYA_String* out = nya_string_create(arena);
    if (v == nullptr) return out;

    switch (v->type) {
        case NYA_TYPE_NULL: break;

        case NYA_TYPE_B8:   nya_string_extend(out, v->as_b8 != 0 ? "true" : "false"); break;
        case NYA_TYPE_B16:  nya_string_extend(out, v->as_b16 != 0 ? "true" : "false"); break;
        case NYA_TYPE_B32:  nya_string_extend(out, v->as_b32 != 0 ? "true" : "false"); break;
        case NYA_TYPE_B64:  nya_string_extend(out, v->as_b64 != 0 ? "true" : "false"); break;
        case NYA_TYPE_B128: nya_string_extend(out, v->as_b128 != 0 ? "true" : "false"); break;

        case NYA_TYPE_U8:  nya_string_extend_sprintf(out, FMTu8, v->as_u8); break;
        case NYA_TYPE_U16: nya_string_extend_sprintf(out, FMTu16, v->as_u16); break;
        case NYA_TYPE_U32: nya_string_extend_sprintf(out, FMTu32, v->as_u32); break;
        case NYA_TYPE_U64: nya_string_extend_sprintf(out, FMTu64, v->as_u64); break;

        case NYA_TYPE_S8:  nya_string_extend_sprintf(out, FMTs8, v->as_s8); break;
        case NYA_TYPE_S16: nya_string_extend_sprintf(out, FMTs16, v->as_s16); break;
        case NYA_TYPE_S32: nya_string_extend_sprintf(out, FMTs32, v->as_s32); break;
        case NYA_TYPE_S64: nya_string_extend_sprintf(out, FMTs64, v->as_s64); break;

        case NYA_TYPE_U128: nya_string_extend(out, nya_u128_to_string(arena, v->as_u128)); break;
        case NYA_TYPE_S128: nya_string_extend(out, nya_s128_to_string(arena, v->as_s128)); break;

        case NYA_TYPE_F16: nya_string_extend_sprintf(out, "%.5g", (f64)v->as_f16); break;
        case NYA_TYPE_F32: nya_string_extend_sprintf(out, "%.9g", (f64)v->as_f32); break;
        case NYA_TYPE_F64: nya_string_extend_sprintf(out, "%.17g", v->as_f64); break;
        case NYA_TYPE_F128: nya_string_extend_sprintf(out, "%.17g", (f64)v->as_f128); break;

        case NYA_TYPE_CHAR: nya_string_push_back(out, (u8)v->as_char); break;

        case NYA_TYPE_STRING: nya_string_extend(out, v->as_string != nullptr ? v->as_string : ""); break;

        // Objects, arrays, wide strings and raw pointers have no natural rendering, so they interpolate as nothing rather than an address someone might trust.
        default: break;
    }

    return out;
}

/** Reads a value as an integer where it is one, for the date filter. */
NYA_INTERNAL b8 _nya_template_value_as_s64(const NYA_Value* v, OUT s64* out) {
    if (v == nullptr) return false;
    switch (v->type) {
        case NYA_TYPE_U8:  *out = (s64)v->as_u8; return true;
        case NYA_TYPE_U16: *out = (s64)v->as_u16; return true;
        case NYA_TYPE_U32: *out = (s64)v->as_u32; return true;
        case NYA_TYPE_U64: *out = (s64)v->as_u64; return true;
        case NYA_TYPE_S8:  *out = (s64)v->as_s8; return true;
        case NYA_TYPE_S16: *out = (s64)v->as_s16; return true;
        case NYA_TYPE_S32: *out = (s64)v->as_s32; return true;
        case NYA_TYPE_S64: *out = v->as_s64; return true;
        default:           return false;
    }
}

/** Whether a value is true in a condition. Undefined and null are false; empty strings and empty
 *  collections are false; a number is its non-zeroness. */
NYA_INTERNAL b8 _nya_template_truthy(const NYA_Value* v) {
    if (v == nullptr) return false;
    switch (v->type) {
        case NYA_TYPE_NULL: return false;

        case NYA_TYPE_B8:   return v->as_b8 != 0;
        case NYA_TYPE_B16:  return v->as_b16 != 0;
        case NYA_TYPE_B32:  return v->as_b32 != 0;
        case NYA_TYPE_B64:  return v->as_b64 != 0;
        case NYA_TYPE_B128: return v->as_b128 != 0;

        case NYA_TYPE_U8:  return v->as_u8 != 0;
        case NYA_TYPE_U16: return v->as_u16 != 0;
        case NYA_TYPE_U32: return v->as_u32 != 0;
        case NYA_TYPE_U64: return v->as_u64 != 0;
        case NYA_TYPE_U128: return v->as_u128 != 0;
        case NYA_TYPE_S8:  return v->as_s8 != 0;
        case NYA_TYPE_S16: return v->as_s16 != 0;
        case NYA_TYPE_S32: return v->as_s32 != 0;
        case NYA_TYPE_S64: return v->as_s64 != 0;
        case NYA_TYPE_S128: return v->as_s128 != 0;

        case NYA_TYPE_F16:  return v->as_f16 != 0;
        case NYA_TYPE_F32:  return v->as_f32 != 0;
        case NYA_TYPE_F64:  return v->as_f64 != 0;
        case NYA_TYPE_F128: return v->as_f128 != 0;

        case NYA_TYPE_CHAR:   return v->as_char != 0;
        case NYA_TYPE_STRING: return v->as_string != nullptr && v->as_string[0] != '\0';

        case NYA_TYPE_OBJECT: return v->as_object.length > 0;
        case NYA_TYPE_ARRAY:  return v->as_array.length > 0;

        default: return true;
    }
}

/*
 * ───────────────────────────────────── PATH RESOLUTION ─────────────────────────────────────
 *
 * A path is dot separated: `user.address.city`, `items.0.title`. The first segment names a loop
 * variable in scope, the reserved `forloop` cursor inside a loop, or a key of the root context. Each
 * further segment steps into an object by key or into an array by index. A step that does not fit —
 * a key that is not there, an index past the end, a segment against a scalar — is undefined, and
 * undefined is not a crash: it renders empty, or takes a `default`.
 */

/** Whether a slice is a run of ASCII digits, and its value if so. */
NYA_INTERNAL b8 _nya_template_slice_index(_NyaTemplateSlice s, OUT u64* out) {
    if (s.length == 0) return false;
    u64 value = 0;
    for (u64 i = 0; i < s.length; i++) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') return false;
        value = value * 10 + (u64)(s.ptr[i] - '0');
    }
    *out = value;
    return true;
}

/** Takes the segment before the next '.', advancing `path` past it. */
NYA_INTERNAL _NyaTemplateSlice _nya_template_next_segment(_NyaTemplateSlice* path) {
    u64 i = 0;
    while (i < path->length && path->ptr[i] != '.') i++;

    _NyaTemplateSlice seg = { .ptr = path->ptr, .length = i };
    if (i < path->length) i++; // skip the dot
    path->ptr    += i;
    path->length -= i;
    return seg;
}

/** Resolves `field` of the innermost loop's cursor into `scratch`. */
NYA_INTERNAL b8 _nya_template_resolve_forloop(_NyaTemplate* t, _NyaTemplateSlice field, OUT NYA_Value* scratch) {
    if (t->scope_count == 0) return false;
    const _NyaTemplateScope* loop = &t->scope[t->scope_count - 1];

    if (_nya_template_slice_eq(field, "index"))    { *scratch = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = loop->index0 + 1 }; return true; }
    if (_nya_template_slice_eq(field, "index0"))   { *scratch = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = loop->index0 }; return true; }
    if (_nya_template_slice_eq(field, "revindex")) { *scratch = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = loop->length - loop->index0 }; return true; }
    if (_nya_template_slice_eq(field, "revindex0")){ *scratch = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = loop->length - loop->index0 - 1 }; return true; }
    if (_nya_template_slice_eq(field, "length"))   { *scratch = (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = loop->length }; return true; }
    if (_nya_template_slice_eq(field, "first"))    { *scratch = (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = loop->index0 == 0 }; return true; }
    if (_nya_template_slice_eq(field, "last"))     { *scratch = (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = loop->index0 == loop->length - 1 }; return true; }

    return false;
}

/**
 * Resolves a path to a value. On a hit `*out` points at either an existing value (owned by the
 * context or a loop element) or at `scratch`, which the caller supplies for synthesised values.
 * */
NYA_INTERNAL b8 _nya_template_resolve(_NyaTemplate* t, _NyaTemplateSlice path, OUT NYA_Value* scratch, OUT const NYA_Value** out) {
    path = _nya_template_trim(path);
    if (path.length == 0) return false;

    _NyaTemplateSlice first = _nya_template_next_segment(&path);
    const NYA_Value*  base  = nullptr;

    // A loop variable in scope shadows the context, innermost first.
    for (s64 i = (s64)t->scope_count - 1; i >= 0; i--) {
        if (_nya_template_slice_eq(first, t->scope[i].name)) {
            base = t->scope[i].value;
            break;
        }
    }

    // The reserved cursor: `forloop.index`, `forloop.first`, ...
    if (base == nullptr && _nya_template_slice_eq(first, "forloop")) {
        _NyaTemplateSlice field = _nya_template_next_segment(&path);
        if (path.length != 0) return false; // `forloop.index.x` is nothing
        if (!_nya_template_resolve_forloop(t, field, scratch)) return false;
        *out = scratch;
        return true;
    }

    // Otherwise a key of the root context.
    if (base == nullptr) {
        if (t->context == nullptr) return false;
        if (first.length >= NYA_TEMPLATE_MAX_SEGMENT) return false;
        char key[NYA_TEMPLATE_MAX_SEGMENT];
        memcpy(key, first.ptr, first.length);
        key[first.length] = '\0';
        base = nya_object_get(t->context, key);
    }

    if (base == nullptr) return false;

    // Walk the remaining segments into objects and arrays.
    while (path.length > 0) {
        _NyaTemplateSlice seg = _nya_template_next_segment(&path);

        if (base->type == NYA_TYPE_OBJECT) {
            if (seg.length >= NYA_TEMPLATE_MAX_SEGMENT) return false;
            char key[NYA_TEMPLATE_MAX_SEGMENT];
            memcpy(key, seg.ptr, seg.length);
            key[seg.length] = '\0';
            base            = nya_object_get(&base->as_object, key);
            if (base == nullptr) return false;
        } else if (base->type == NYA_TYPE_ARRAY) {
            u64 index = 0;
            if (!_nya_template_slice_index(seg, &index)) return false;
            if (index >= base->as_array.length) return false;
            base = &base->as_array.items[index];
        } else {
            return false;
        }
    }

    *out = base;
    return true;
}

// ───────────────────────────────────── DATE FILTER ─────────────────────────────────────

/** Formats an instant (nanoseconds since the Unix epoch) per `format`: "date", "time", else RFC 3339. */
NYA_INTERNAL NYA_String* _nya_template_format_date(NYA_Arena* arena, s64 ns, _NyaTemplateSlice format) {
    NYA_Instant instant = { .ns = ns };
    NYA_String* out     = nya_string_create(arena);

    if (_nya_template_slice_eq(format, "date")) {
        NYA_Date     date = { 0 };
        NYA_TimeOfDay clock = { 0 };
        nya_instant_to_utc(instant, &date, &clock);
        nya_string_extend_sprintf(out, "%04d-%02u-%02u", date.year, date.month, date.day);
    } else if (_nya_template_slice_eq(format, "time")) {
        NYA_Date     date = { 0 };
        NYA_TimeOfDay clock = { 0 };
        nya_instant_to_utc(instant, &date, &clock);
        nya_string_extend_sprintf(out, "%02u:%02u:%02u", clock.hour, clock.minute, clock.second);
    } else {
        u8  buffer[NYA_RFC3339_LENGTH_MAX + 1];
        u32 written = nya_instant_to_rfc3339(instant, buffer, sizeof(buffer));
        nya_string_extend(out, (NYA_ConstCString)buffer);
        (void)written;
    }

    return out;
}

// ───────────────────────────────────── INTERPOLATION ─────────────────────────────────────

/** Reads `name` or `name:arg` from a filter list, advancing `list` past it. */
NYA_INTERNAL _NyaTemplateSlice _nya_template_filter(_NyaTemplateSlice* list, OUT _NyaTemplateSlice* arg) {
    *list = _nya_template_trim(*list);

    u64 i = 0;
    while (i < list->length && list->ptr[i] != ':' && list->ptr[i] != '|') i++;

    _NyaTemplateSlice name = _nya_template_trim((_NyaTemplateSlice){ .ptr = list->ptr, .length = i });
    *arg                   = (_NyaTemplateSlice){ .ptr = nullptr, .length = 0 };

    if (i < list->length && list->ptr[i] == ':') {
        i++; // skip ':'
        // A quoted argument runs to its closing quote; a bare one runs to the next '|'.
        if (i < list->length && (list->ptr[i] == '"' || list->ptr[i] == '\'')) {
            char quote = list->ptr[i];
            i++;
            u64 start = i;
            while (i < list->length && list->ptr[i] != quote) i++;
            *arg = (_NyaTemplateSlice){ .ptr = list->ptr + start, .length = i - start };
            if (i < list->length) i++; // skip closing quote
        } else {
            u64 start = i;
            while (i < list->length && list->ptr[i] != '|') i++;
            *arg = _nya_template_trim((_NyaTemplateSlice){ .ptr = list->ptr + start, .length = i - start });
        }
    }

    if (i < list->length && list->ptr[i] == '|') i++; // skip the separator
    *list = (_NyaTemplateSlice){ .ptr = list->ptr + i, .length = list->length - i };
    return name;
}

/** Renders one `{{ ... }}`: resolve the path, run the filter pipeline, escape unless opted out. */
NYA_INTERNAL NYA_Error _nya_template_render_interp(_NyaTemplate* t, b8 emit, _NyaTemplateSlice inner) {
    // Split the path from the filter list.
    u64 bar = 0;
    while (bar < inner.length && inner.ptr[bar] != '|') bar++;

    _NyaTemplateSlice path    = _nya_template_trim((_NyaTemplateSlice){ .ptr = inner.ptr, .length = bar });
    _NyaTemplateSlice filters = { .ptr = nullptr, .length = 0 };
    if (bar < inner.length) filters = (_NyaTemplateSlice){ .ptr = inner.ptr + bar + 1, .length = inner.length - bar - 1 };

    NYA_Value        scratch = { 0 };
    const NYA_Value* value   = nullptr;
    b8               found   = _nya_template_resolve(t, path, &scratch, &value);

    NYA_String* text = _nya_template_value_to_string(t->arena, found ? value : nullptr);

    // "Empty" for the default filter: undefined, null, or an empty rendering.
    b8 is_empty   = !found || value == nullptr || value->type == NYA_TYPE_NULL || text->length == 0;
    b8 autoescape = t->mode != NYA_TEMPLATE_ESCAPE_NONE;

    // Run the pipeline left to right.
    while (filters.length > 0) {
        _NyaTemplateSlice arg;
        _NyaTemplateSlice name = _nya_template_filter(&filters, &arg);
        if (name.length == 0) continue;

        if (_nya_template_slice_eq(name, "raw")) {
            // Opts the value out of autoescaping. Dangerous: only for values the author trusts in the output language.
            autoescape = false;
        } else if (_nya_template_slice_eq(name, "escape")) {
            // Escapes now and turns autoescaping off so the final step does not double-encode; a NONE render escapes as HTML, the safe reading.
            NYA_TemplateEscape as = t->mode == NYA_TEMPLATE_ESCAPE_NONE ? NYA_TEMPLATE_ESCAPE_HTML : t->mode;
            NYA_String*        escaped = nya_string_create(t->arena);
            _NyaTemplate       sink    = *t;
            sink.out                   = escaped;
            NYA_TRY(_nya_template_put_escaped(&sink, true, text->items, text->length, as));
            text       = escaped;
            autoescape = false;
        } else if (_nya_template_slice_eq(name, "upper")) {
            nya_string_to_upper(text);
        } else if (_nya_template_slice_eq(name, "lower")) {
            nya_string_to_lower(text);
        } else if (_nya_template_slice_eq(name, "default")) {
            if (is_empty) {
                text = nya_string_create(t->arena);
                for (u64 i = 0; i < arg.length; i++) nya_string_push_back(text, (u8)arg.ptr[i]);
                is_empty = false;
            }
        } else if (_nya_template_slice_eq(name, "date")) {
            s64 ns = 0;
            if (found && _nya_template_value_as_s64(value, &ns)) {
                text     = _nya_template_format_date(t->arena, ns, arg);
                is_empty = text->length == 0;
            }
        } else {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown template filter '%.*s'", (int)name.length, name.ptr);
        }
    }

    if (autoescape) return _nya_template_put_escaped(t, emit, text->items, text->length, t->mode);
    return _nya_template_put(t, emit, text->items, text->length);
}

// ───────────────────────────────────── CONTROL BLOCKS ─────────────────────────────────────

/** `{% if path %}...{% else %}...{% endif %}`. `cursor` starts just after the opening tag. */
NYA_INTERNAL NYA_Error _nya_template_render_if(_NyaTemplate* t, b8 emit, u32 depth, _NyaTemplateSlice condition, const char** cursor) {
    b8 taken = false;
    if (emit) {
        NYA_Value        scratch = { 0 };
        const NYA_Value* value   = nullptr;
        taken                    = _nya_template_resolve(t, condition, &scratch, &value) && _nya_template_truthy(value);
    }

    _NyaTemplateStop stop;
    NYA_TRY(_nya_template_render_block(t, emit && taken, depth + 1, cursor, &stop));

    if (stop == _NYA_TEMPLATE_STOP_ELSE) {
        NYA_TRY(_nya_template_render_block(t, emit && !taken, depth + 1, cursor, &stop));
    }

    if (stop != _NYA_TEMPLATE_STOP_ENDIF) return nya_error(NYA_ERROR_PARSE, "unbalanced '{%% if %%}': expected '{%% endif %%}'");
    return NYA_OK;
}

/** `{% for name in path %}...{% endfor %}`. `cursor` starts just after the opening tag. */
NYA_INTERNAL NYA_Error _nya_template_render_for(_NyaTemplate* t, b8 emit, u32 depth, _NyaTemplateSlice header, const char** cursor) {
    _NyaTemplateSlice rest;
    _NyaTemplateSlice name = _nya_template_word(header, &rest);
    _NyaTemplateSlice in   = _nya_template_word(rest, &rest);
    _NyaTemplateSlice list = rest;

    if (name.length == 0 || !_nya_template_slice_eq(in, "in") || list.length == 0) {
        return nya_error(NYA_ERROR_PARSE, "malformed '{%% for %%}': expected 'for <name> in <path>'");
    }

    const char* body = *cursor;

    // Not emitting, or nothing to iterate: walk the body once with the pen up to find the endfor.
    NYA_Value        scratch = { 0 };
    const NYA_Value* value   = nullptr;
    b8               have    = emit && _nya_template_resolve(t, list, &scratch, &value) && value != nullptr && value->type == NYA_TYPE_ARRAY;

    if (!have || value->as_array.length == 0) {
        _NyaTemplateStop stop;
        NYA_TRY(_nya_template_render_block(t, false, depth + 1, cursor, &stop));
        if (stop != _NYA_TEMPLATE_STOP_ENDFOR) return nya_error(NYA_ERROR_PARSE, "unbalanced '{%% for %%}': expected '{%% endfor %%}'");
        return NYA_OK;
    }

    if (t->scope_count >= NYA_TEMPLATE_MAX_DEPTH) return nya_error(NYA_ERROR_NOT_SUPPORTED, "template loop nesting exceeds %u", NYA_TEMPLATE_MAX_DEPTH);

    // The loop variable's name, kept for the lifetime of the loop.
    if (name.length >= NYA_TEMPLATE_MAX_SEGMENT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "loop variable name too long");
    char* name_z = nya_arena_alloc(t->arena, name.length + 1);
    memcpy(name_z, name.ptr, name.length);
    name_z[name.length] = '\0';

    u64 length = value->as_array.length;
    for (u64 i = 0; i < length; i++) {
        if (++t->iterations > NYA_TEMPLATE_MAX_ITERATIONS) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "template loop iterations exceed %u", NYA_TEMPLATE_MAX_ITERATIONS);
        }

        t->scope[t->scope_count++] = (_NyaTemplateScope){
            .name   = name_z,
            .value  = &value->as_array.items[i],
            .index0 = (s64)i,
            .length = (s64)length,
        };

        const char*      body_cursor = body;
        _NyaTemplateStop stop;
        NYA_Error        result = _nya_template_render_block(t, true, depth + 1, &body_cursor, &stop);

        t->scope_count--;
        NYA_TRY(result);
        if (stop != _NYA_TEMPLATE_STOP_ENDFOR) return nya_error(NYA_ERROR_PARSE, "unbalanced '{%% for %%}': expected '{%% endfor %%}'");

        *cursor = body_cursor;
    }

    return NYA_OK;
}

/**
 * Renders from `*cursor` until the tag that closes the current block, or the end of the template.
 * Updates `*cursor` to just past that closing tag and reports which one it was through `*stop`.
 * */
NYA_INTERNAL NYA_Error _nya_template_render_block(_NyaTemplate* t, b8 emit, u32 depth, const char** cursor, OUT _NyaTemplateStop* stop) {
    if (depth > NYA_TEMPLATE_MAX_DEPTH) return nya_error(NYA_ERROR_NOT_SUPPORTED, "template nesting exceeds %u", NYA_TEMPLATE_MAX_DEPTH);

    const char* p   = *cursor;
    const char* end = p + strlen(p);

    while (p < end) {
        // Emit the run of literal text up to the next construct.
        const char* run = p;
        while (p < end && !(p[0] == '{' && (p[1] == '{' || p[1] == '%' || p[1] == '#'))) p++;
        NYA_TRY(_nya_template_put(t, emit, (const u8*)run, (u64)(p - run)));
        if (p >= end) break;

        char kind = p[1];

        if (kind == '{') {
            const char* close = _nya_template_find2(p + 2, end + 1, '}', '}');
            if (close == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated '{{': expected '}}'");
            _NyaTemplateSlice inner = { .ptr = p + 2, .length = (u64)(close - (p + 2)) };
            NYA_TRY(_nya_template_render_interp(t, emit, inner));
            p = close + 2;
        } else if (kind == '#') {
            const char* close = _nya_template_find2(p + 2, end + 1, '#', '}');
            if (close == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated '{#': expected '#}'");
            p = close + 2;
        } else { // kind == '%'
            const char* close = _nya_template_find2(p + 2, end + 1, '%', '}');
            if (close == nullptr) return nya_error(NYA_ERROR_PARSE, "unterminated '{%%': expected '%%}'");

            _NyaTemplateSlice body    = { .ptr = p + 2, .length = (u64)(close - (p + 2)) };
            _NyaTemplateSlice rest;
            _NyaTemplateSlice keyword = _nya_template_word(body, &rest);
            p                         = close + 2;

            if (_nya_template_slice_eq(keyword, "if")) {
                NYA_TRY(_nya_template_render_if(t, emit, depth, rest, &p));
            } else if (_nya_template_slice_eq(keyword, "for")) {
                NYA_TRY(_nya_template_render_for(t, emit, depth, rest, &p));
            } else if (_nya_template_slice_eq(keyword, "endif")) {
                *cursor = p;
                *stop   = _NYA_TEMPLATE_STOP_ENDIF;
                return NYA_OK;
            } else if (_nya_template_slice_eq(keyword, "endfor")) {
                *cursor = p;
                *stop   = _NYA_TEMPLATE_STOP_ENDFOR;
                return NYA_OK;
            } else if (_nya_template_slice_eq(keyword, "else")) {
                *cursor = p;
                *stop   = _NYA_TEMPLATE_STOP_ELSE;
                return NYA_OK;
            } else {
                return nya_error(NYA_ERROR_PARSE, "unknown template tag '{%% %.*s %%}'", (int)keyword.length, keyword.ptr);
            }
        }
    }

    *cursor = p;
    *stop   = _NYA_TEMPLATE_STOP_EOF;
    return NYA_OK;
}

// ───────────────────────────────────── PUBLIC API ─────────────────────────────────────

NYA_Error nya_template_render(
    NYA_Arena* arena, NYA_ConstCString template_text, const NYA_Object* context, NYA_TemplateEscape mode, OUT NYA_String** out_string) {
    nya_assert(arena != nullptr);
    nya_assert(out_string != nullptr);

    if (template_text == nullptr) template_text = "";

    _NyaTemplate t = {
        .arena       = arena,
        .context     = context,
        .mode        = mode,
        .out         = nya_string_create(arena),
        .scope_count = 0,
        .iterations  = 0,
    };

    const char*      cursor = template_text;
    _NyaTemplateStop stop;
    NYA_TRY(_nya_template_render_block(&t, true, 0, &cursor, &stop));

    // A block-closing tag with no block open is as unbalanced as an open block with no close.
    if (stop != _NYA_TEMPLATE_STOP_EOF) return nya_error(NYA_ERROR_PARSE, "template has a closing tag with no matching block");

    *out_string = t.out;
    return NYA_OK;
}
