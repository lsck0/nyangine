#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/http/http_log.h"
#include "nyangine/serde/serde.h"

static_assert(NYA_HTTP_LOG_MAX_RECORD_BYTES <= NYA_LOG_MESSAGE_MAX_LENGTH,
              "a record longer than a log message is a record the formatter cuts where nobody decided to");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The headers no level ever writes, lowercased as NYA_HttpRequest stores them.
 *
 * Every one of them is a credential in full: the two authorization headers carry a token, and a cookie
 * jar carries the session both ways. There is no configuration that turns these back on, because a
 * server that can be configured into logging its own session cookies is a server that eventually is.
 * */
NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_LOG_DENIED[] = { "authorization", "cookie", "set-cookie", "proxy-authorization" };

/**
 * Words that make a query parameter a secret whatever the route calls it.
 *
 * A query string carries no type, so there is no reflection to ask and the name is the only evidence
 * there is. The same four words are what src/build/lint.c refuses a reflected field for when it carries
 * no `@redact`, so a name that would fail the check also never reaches a log.
 * */
NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_LOG_SECRET_WORDS[] = { "password", "token", "secret", "code" };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What nya_http_log_config_set installed. One server per process, so one of these. */
NYA_INTERNAL NYA_HttpLogConfig _NYA_HTTP_LOG_CONFIG = { 0 };

/** A bounded record under construction. Everything past `capacity` is dropped, and `overflowed` says so. */
typedef struct {
    char buffer[NYA_HTTP_LOG_MAX_RECORD_BYTES];
    u64  length;
    b8   overflowed;
} _NYA_HttpLogRecord;

/** Appends to a record, bounded. A write that does not fit sets `overflowed` and is dropped whole. */
NYA_INTERNAL void _nya_http_log_append(_NYA_HttpLogRecord* record, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/** Whether `text` holds `word`, neither of them case sensitive. */
NYA_INTERNAL b8 _nya_http_log_contains_word(NYA_ConstCString text, NYA_ConstCString word) __attr_no_discard;

/** Whether the comma separated `list` names `name`, ignoring case and surrounding spaces. */
NYA_INTERNAL b8 _nya_http_log_list_names(NYA_ConstCString list, const char* name, u64 name_size) __attr_no_discard;

/** Whether `type` has a `@redact` field called `name`, at any depth. Null type is false. */
NYA_INTERNAL b8 _nya_http_log_type_redacts(const NYA_TypeReflection* type, const char* name, u64 name_size, u32 depth) __attr_no_discard;

/** Whether a query parameter called `name` is a secret. See the three rules in the header. */
NYA_INTERNAL b8 _nya_http_log_query_is_secret(const NYA_HttpRoute* route, const char* name, u64 name_size) __attr_no_discard;

/** `size` bytes as "N bytes, blake2b <hex>". The fail-closed form of a body. */
NYA_INTERNAL void _nya_http_log_fingerprint(const u8* data, u64 size, OUT char* out, u64 capacity);

/**
 * One body, decoded through `type` and redacted, or its fingerprint when anything at all is off.
 * */
NYA_INTERNAL void _nya_http_log_body(
    NYA_Arena*                arena,
    const NYA_TypeReflection* type,
    NYA_HttpMediaType         media,
    const u8*                 data,
    u64                       size,
    OUT char*                 out,
    u64                       capacity
);

/** The headers of a request, and then of the response, appended to the record with the deny list applied. */
NYA_INTERNAL void _nya_http_log_headers(_NYA_HttpLogRecord* record, NYA_ConstCString label, const NYA_HttpHeader* headers, u32 count);

/** The query string with every secret parameter's value replaced. Empty when there is no query. */
NYA_INTERNAL void _nya_http_log_query(const NYA_HttpExchange* exchange, OUT char* out, u64 capacity);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_http_log_config_set(NYA_HttpLogConfig config) {
    // a level outside the enum is a config file with a name this build does not know; the summary is
    // the safe reading of it, since it is the level that carries the least.
    if ((u32)config.level >= (u32)NYA_HTTP_LOG_LEVEL_COUNT) config.level = NYA_HTTP_LOG_SUMMARY;
    if ((u32)config.address >= (u32)NYA_HTTP_LOG_ADDRESS_COUNT) config.address = NYA_HTTP_LOG_ADDRESS_NETWORK;

    _NYA_HTTP_LOG_CONFIG = config;
}

NYA_HttpLogConfig nya_http_log_config_get(void) {
    return _NYA_HTTP_LOG_CONFIG;
}

b8 nya_http_log_header_is_denied(NYA_ConstCString name) {
    if (name == nullptr || name[0] == '\0') return false;

    u64 size = strnlen(name, NYA_HTTP_MAX_HEADER_NAME);

    for (u32 index = 0; index < nya_carray_length(_NYA_HTTP_LOG_DENIED); index++) {
        if (_nya_http_log_list_names(_NYA_HTTP_LOG_DENIED[index], name, size)) return true;
    }

    return _nya_http_log_list_names(_NYA_HTTP_LOG_CONFIG.deny, name, size);
}

NYA_HttpStatus nya_http_layer_log(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    u64 started_ns = nya_clock_get_monotonic_ns();

    NYA_HttpStatus status = nya_http_chain_next(exchange, next);

    u64 elapsed_us = (nya_clock_get_monotonic_ns() - started_ns) / 1000;

    char address[NYA_HTTP_MAX_ADDRESS] = { 0 };

    switch (_NYA_HTTP_LOG_CONFIG.address) {
        case NYA_HTTP_LOG_ADDRESS_FULL:
            (void)snprintf(address, sizeof(address), "%s", exchange->address != nullptr ? exchange->address : "");
            break;

        case NYA_HTTP_LOG_ADDRESS_NONE: (void)snprintf(address, sizeof(address), "%s", "-"); break;

        case NYA_HTTP_LOG_ADDRESS_NETWORK:
        case NYA_HTTP_LOG_ADDRESS_COUNT:
        default: nya_http_address_truncate(exchange->address != nullptr ? exchange->address : "", address, sizeof(address)); break;
    }

    _NYA_HttpLogRecord record = { 0 };

    /*
     * The route's own path, never the request's: that is the caller's text, query string and all, and a
     * token in a query would land in the log. An unmatched request is logged as such for the same
     * reason. The request id is not here because the server's log tag already puts it on this line.
     */
    _nya_http_log_append(
        &record,
        "%s %s -> %d (%llu us, %llu in, %llu out) from %s as %s",
        nya_http_method_text(exchange->request->method),
        exchange->route != nullptr ? exchange->route->path : "(no route)",
        (s32)status,
        (unsigned long long)elapsed_us,
        (unsigned long long)exchange->request->body_size,
        (unsigned long long)exchange->response->body_size,
        address,
        exchange->identified ? exchange->identity.subject : "-"
    );

    if (_NYA_HTTP_LOG_CONFIG.level >= NYA_HTTP_LOG_HEADERS) {
        char query[NYA_HTTP_LOG_MAX_VALUE_BYTES * 2] = { 0 };
        _nya_http_log_query(exchange, query, sizeof(query));

        if (query[0] != '\0') _nya_http_log_append(&record, "\n  ?%s", query);

        _nya_http_log_headers(&record, "<", exchange->request->headers, exchange->request->header_count);
        _nya_http_log_headers(&record, ">", exchange->response->headers, exchange->response->header_count);
    }

    if (_NYA_HTTP_LOG_CONFIG.level >= NYA_HTTP_LOG_BODIES) {
        char body[NYA_HTTP_LOG_MAX_BODY_BYTES + 32] = { 0 };

        _nya_http_log_body(
            exchange->arena,
            exchange->route != nullptr ? exchange->route->request_type : nullptr,
            exchange->request->media_type,
            exchange->request->body,
            exchange->request->body_size,
            body,
            sizeof(body)
        );
        _nya_http_log_append(&record, "\n  < %s", body);

        _nya_http_log_body(
            exchange->arena,
            exchange->route != nullptr ? exchange->route->response_type : nullptr,
            exchange->response->media_type,
            exchange->response->body,
            exchange->response->body_size,
            body,
            sizeof(body)
        );
        _nya_http_log_append(&record, "\n  > %s", body);
    }

    if (record.overflowed) _nya_http_log_append(&record, "%s", "\n  ...");

    /*
     * One call, and the first moment any of this leaves this function. Everything above substituted
     * rather than filtered, so there is no version of the record holding a secret for a sink to be
     * trusted with: whatever the file sink writes is what the ring holds and what a crash report
     * prints, because it is the same bytes.
     */
    nya_log_info("%s", record.buffer);

    return status;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_http_log_config_apply(void* instance) {
    if (instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no config to apply");

    nya_http_log_config_set(*(const NYA_HttpLogConfig*)instance);

    return NYA_OK;
}

void _nya_http_log_append(_NYA_HttpLogRecord* record, NYA_ConstCString format, ...) {
    nya_assert(record != nullptr && format != nullptr);

    if (record->length + 1 >= sizeof(record->buffer)) {
        record->overflowed = true;
        return;
    }

    u64 room = sizeof(record->buffer) - record->length;

    va_list args;
    va_start(args, format);
    s32 written = vsnprintf(record->buffer + record->length, room, format, args);
    va_end(args);

    if (written < 0) return;

    // a write that did not fit is dropped whole rather than half written: half a header line reads as a
    // shorter value, and a reader has no way to tell that from the real one.
    if ((u64)written >= room) {
        record->buffer[record->length] = '\0';
        record->overflowed             = true;
        return;
    }

    record->length += (u64)written;
}

b8 _nya_http_log_contains_word(NYA_ConstCString text, NYA_ConstCString word) {
    u64 size = strlen(word);

    for (u64 i = 0; text[i] != '\0'; i++) {
        u64 match = 0;

        while (match < size && text[i + match] != '\0' && _nya_http_lower(text[i + match]) == word[match]) match++;

        if (match == size) return true;
    }

    return false;
}

b8 _nya_http_log_list_names(NYA_ConstCString list, const char* name, u64 name_size) {
    if (list == nullptr || list[0] == '\0' || name == nullptr) return false;

    u64 cursor = 0;

    while (list[cursor] != '\0') {
        while (list[cursor] == ',' || list[cursor] == ' ') cursor++;

        u64 start = cursor;
        while (list[cursor] != '\0' && list[cursor] != ',') cursor++;

        u64 end = cursor;
        while (end > start && list[end - 1] == ' ') end--;

        if (end - start != name_size) continue;

        b8 same = true;
        for (u64 i = 0; i < name_size && same; i++) same = _nya_http_lower(list[start + i]) == _nya_http_lower(name[i]);

        if (same) return true;
    }

    return false;
}

b8 _nya_http_log_type_redacts(const NYA_TypeReflection* type, const char* name, u64 name_size, u32 depth) {
    // the tables are generated and const, so a walk this deep is a broken table rather than a deep type.
    if (type == nullptr || depth >= NYA_REFLECT_LAYOUT_DEPTH_MAX) return false;
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) return false;

    for (u32 index = 0; index < type->field_count; index++) {
        const NYA_ReflectField* field = &type->fields[index];

        if (field->name == nullptr) continue;

        if (field->is_redacted && strlen(field->name) == name_size && nya_memcmp(field->name, name, name_size) == 0) return true;

        if (_nya_http_log_type_redacts(field->type, name, name_size, depth + 1)) return true;
    }

    return false;
}

b8 _nya_http_log_query_is_secret(const NYA_HttpRoute* route, const char* name, u64 name_size) {
    if (name_size == 0 || name_size >= NYA_HTTP_MAX_HEADER_NAME) return true;

    if (_nya_http_log_list_names(_NYA_HTTP_LOG_CONFIG.deny, name, name_size)) return true;

    char text[NYA_HTTP_MAX_HEADER_NAME] = { 0 };
    nya_memcpy(text, name, name_size);

    for (u32 index = 0; index < nya_carray_length(_NYA_HTTP_LOG_SECRET_WORDS); index++) {
        if (_nya_http_log_contains_word(text, _NYA_HTTP_LOG_SECRET_WORDS[index])) return true;
    }

    // and what the route's own request type says, which is the only structural evidence a query has:
    // a parameter spelled like a `@redact` field of the DTO is the same value by another route in.
    return route != nullptr && _nya_http_log_type_redacts(route->request_type, name, name_size, 0);
}

void _nya_http_log_fingerprint(const u8* data, u64 size, OUT char* out, u64 capacity) {
    u8 hash[NYA_HTTP_LOG_HASH_DIGITS / 2] = { 0 };

    nya_crypto_blake2b(data, size, hash, sizeof(hash));

    char hex[NYA_HTTP_LOG_HASH_DIGITS + 1] = { 0 };
    for (u64 index = 0; index < sizeof(hash); index++) (void)snprintf(hex + index * 2, 3, "%02x", hash[index]);

    (void)snprintf(out, capacity, FMTu64 " bytes, blake2b %s", size, hex);
}

void _nya_http_log_body(
    NYA_Arena*                arena,
    const NYA_TypeReflection* type,
    NYA_HttpMediaType         media,
    const u8*                 data,
    u64                       size,
    OUT char*                 out,
    u64                       capacity
) {
    out[0] = '\0';

    if (size == 0) {
        (void)snprintf(out, capacity, "%s", "(empty)");
        return;
    }

    // every way of not knowing what these bytes are ends in the same place, which is the point: a route
    // with no DTO, a format the route did not declare, and a body that is not a document at all.
    if (arena == nullptr || type == nullptr || type->kind != NYA_REFLECT_STRUCT) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    /*
     * A type carrying `@on_apply` is refused rather than round tripped: reading a document into one runs
     * the program's own apply function, and logging a request must not run anything. No route DTO has
     * one; this is here so that none can be given one by accident.
     */
    if (type->on_apply != nullptr) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    NYA_Object* document = nullptr;
    NYA_Error   parsed   = nya_error(NYA_ERROR_PARSE, "not a document this route declared");

    switch (media) {
        case NYA_HTTP_MEDIA_JSON: parsed = nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &document); break;
        case NYA_HTTP_MEDIA_NYA:  parsed = nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &document); break;

        // the binary form decodes against this very type, so a peer built from other headers is refused
        // by its layout hash rather than read into the wrong fields and logged as those.
        case NYA_HTTP_MEDIA_NYA_BINARY: parsed = nya_serde_nya_binary_decode(arena, data, size, type, &document); break;

        default: break;
    }

    if (!parsed.ok || document == nullptr) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    // fail closed on a key this type has no field for: an unknown key is a value nothing described, and
    // a value nothing described is a value nothing could have tagged `@redact`.
    if (nya_reflect_check(type, document, nullptr, nullptr) != 0) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    void* instance = nya_arena_alloc(arena, type->size);
    nya_memset(instance, 0, type->size);

    if (!nya_reflect_from_object(type, instance, document).ok) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    NYA_Object* redacted = nya_reflect_to_object_redacted(arena, type, instance);
    NYA_String* text     = redacted != nullptr ? nya_serialize(arena, redacted, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE) : nullptr;

    if (text == nullptr) {
        _nya_http_log_fingerprint(data, size, out, capacity);
        return;
    }

    u64 shown = text->length < NYA_HTTP_LOG_MAX_BODY_BYTES ? text->length : NYA_HTTP_LOG_MAX_BODY_BYTES;

    (void)snprintf(out, capacity, "%.*s%s", (int)shown, text->items, shown < text->length ? "..." : "");
}

void _nya_http_log_headers(_NYA_HttpLogRecord* record, NYA_ConstCString label, const NYA_HttpHeader* headers, u32 count) {
    for (u32 index = 0; index < count && index < NYA_HTTP_MAX_HEADERS; index++) {
        const NYA_HttpHeader* header = &headers[index];

        if (nya_http_log_header_is_denied(header->name)) {
            _nya_http_log_append(record, "\n  %s %s: %s", label, header->name, NYA_REFLECT_REDACTED);
            continue;
        }

        _nya_http_log_append(record, "\n  %s %s: %.*s", label, header->name, NYA_HTTP_LOG_MAX_VALUE_BYTES, header->value);
    }
}

void _nya_http_log_query(const NYA_HttpExchange* exchange, OUT char* out, u64 capacity) {
    out[0] = '\0';

    const NYA_Url* target = &exchange->request->target;
    if (!target->has_query || target->query.length == 0) return;

    const char* query = target->text + target->query.offset;
    u64         size  = target->query.length;

    u64 cursor = 0;
    u64 length = 0;

    while (cursor < size && length + 1 < capacity) {
        u64 start = cursor;
        while (cursor < size && query[cursor] != '&') cursor++;

        u64 pair = cursor - start;
        u64 name = 0;
        while (name < pair && query[start + name] != '=') name++;

        // the value never goes through unexamined: a name with no '=' carries nothing, and a name that
        // is or contains a secret word carries something nobody meant to write down.
        b8 secret = _nya_http_log_query_is_secret(exchange->route, query + start, name);

        s32 written = snprintf(
            out + length,
            capacity - length,
            "%s%.*s=%s",
            length > 0 ? "&" : "",
            (int)name,
            query + start,
            secret ? NYA_REFLECT_REDACTED : ""
        );

        if (written < 0 || (u64)written >= capacity - length) {
            out[length] = '\0';
            return;
        }

        length += (u64)written;

        if (!secret && name < pair) {
            u64 value = pair - name - 1;
            if (value > NYA_HTTP_LOG_MAX_VALUE_BYTES) value = NYA_HTTP_LOG_MAX_VALUE_BYTES;

            written = snprintf(out + length, capacity - length, "%.*s", (int)value, query + start + name + 1);

            if (written < 0 || (u64)written >= capacity - length) {
                out[length] = '\0';
                return;
            }

            length += (u64)written;
        }

        cursor++;
    }
}
