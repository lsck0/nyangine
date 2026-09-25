#include "nyangine-std/base/base_basic.h"
#include "nyangine-core/nyangine.h"

// PRIVATE API DECLARATION

/**
 * The ring, one per thread.
 *
 * `depth` counts every entry ever registered and still live, so it can pass NYA_WATCH_RING_MAX; `lost`
 * is how many of the oldest the ring has since overwritten. Everything readable is the half open range
 * [lost, depth), which is what makes an overflowed ring report a short list rather than a wrong one.
 *
 * Not registered with nya_ceiling_register: the registry holds one pointer per ceiling, and this
 * counter is per thread, so it would report whichever thread happened to register first. What a
 * reader needs is in the report itself, as the number of dropped entries.
 * */
NYA_INTERNAL thread_local NYA_WatchEntry _nya_watch_ring[NYA_WATCH_RING_MAX] = { 0 };
NYA_INTERNAL thread_local u32            _nya_watch_depth                    = 0;
NYA_INTERNAL thread_local u32            _nya_watch_lost                     = 0;

/** Below this a pointer is a null pointer somebody added an offset to, never a string to read. */
#define _NYA_WATCH_NULL_PAGE 4096

/** snprintf into a fresh buffer. Returns what landed there, which is never more than `capacity - 1`. */
NYA_INTERNAL u32 _nya_watch_write(OUT u8* buffer, u32 capacity, NYA_ConstCString format, ...) __attr_fmt_printf(3, 4);

/** The `size` bytes at `address` as one integer, copied out rather than dereferenced through a cast. */
NYA_INTERNAL u128 _nya_watch_read_unsigned(const void* address, u32 size) __attr_no_discard;
NYA_INTERNAL s128 _nya_watch_read_signed(const void* address, u32 size) __attr_no_discard;
NYA_INTERNAL f128 _nya_watch_read_float(const void* address, u32 size) __attr_no_discard;

/** A 128 bit magnitude in decimal, which no printf conversion covers. */
NYA_INTERNAL u32 _nya_watch_format_integer(u128 magnitude, b8 negative, OUT u8* buffer, u32 capacity);

/** `count` bytes of text, quoted and cut, with no read past the first terminator. */
NYA_INTERNAL u32 _nya_watch_format_text(const u8* text, u64 length, OUT u8* buffer, u32 capacity);

// VALUES

u32 _nya_watch_write(OUT u8* buffer, u32 capacity, NYA_ConstCString format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const s32 written = vsnprintf((char*)buffer, capacity, format, arguments);
    va_end(arguments);

    if (written <= 0) {
        buffer[0] = '\0';
        return 0;
    }

    return (u32)written < capacity ? (u32)written : capacity - 1;
}

u128 _nya_watch_read_unsigned(const void* address, u32 size) {
    switch (size) {
        case 1:  { u8 value = 0;  nya_memcpy(&value, address, sizeof(value)); return (u128)value; }
        case 2:  { u16 value = 0; nya_memcpy(&value, address, sizeof(value)); return (u128)value; }
        case 4:  { u32 value = 0; nya_memcpy(&value, address, sizeof(value)); return (u128)value; }
        case 8:  { u64 value = 0; nya_memcpy(&value, address, sizeof(value)); return (u128)value; }
        case 16: { u128 value = 0; nya_memcpy(&value, address, sizeof(value)); return value; }
        default: return 0;
    }
}

s128 _nya_watch_read_signed(const void* address, u32 size) {
    switch (size) {
        case 1:  { s8 value = 0;  nya_memcpy(&value, address, sizeof(value)); return (s128)value; }
        case 2:  { s16 value = 0; nya_memcpy(&value, address, sizeof(value)); return (s128)value; }
        case 4:  { s32 value = 0; nya_memcpy(&value, address, sizeof(value)); return (s128)value; }
        case 8:  { s64 value = 0; nya_memcpy(&value, address, sizeof(value)); return (s128)value; }
        case 16: { s128 value = 0; nya_memcpy(&value, address, sizeof(value)); return value; }
        default: return 0;
    }
}

f128 _nya_watch_read_float(const void* address, u32 size) {
    // By size rather than by type, so f16 and long double land on the right arm on both targets.
    if (size == sizeof(f16)) {
        f16 value = 0;
        nya_memcpy(&value, address, sizeof(value));
        return (f128)value;
    }
    if (size == sizeof(f32)) {
        f32 value = 0;
        nya_memcpy(&value, address, sizeof(value));
        return (f128)value;
    }
    if (size == sizeof(f64)) {
        f64 value = 0;
        nya_memcpy(&value, address, sizeof(value));
        return (f128)value;
    }
    if (size == sizeof(f128)) {
        f128 value = 0;
        nya_memcpy(&value, address, sizeof(value));
        return value;
    }

    return 0;
}

u32 _nya_watch_format_integer(u128 magnitude, b8 negative, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 1);

    // 39 digits is the widest u128, plus a sign and a terminator.
    u8  digits[42] = { 0 };
    u32 length     = 0;

    do {
        digits[length++] = (u8)('0' + (u8)(magnitude % 10));
        magnitude       /= 10;
    } while (magnitude > 0 && length < sizeof(digits) - 1);

    if (negative) digits[length++] = '-';

    u32 written = 0;
    while (written < length && written + 1 < capacity) {
        buffer[written] = digits[length - written - 1];
        written++;
    }

    buffer[written] = '\0';
    return written;
}

u32 _nya_watch_format_text(const u8* text, u64 length, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 1);

    u32 written = 0;

    if (written + 1 < capacity) buffer[written++] = '"';

    for (u64 i = 0; i < length && i < NYA_WATCH_STRING_MAX && written + 1 < capacity; i++) {
        // Control bytes become a dot: the report is read as one line per value, and a stray newline or escape from a string being built would take the line apart.
        const u8 byte   = text[i] >= 0x20 && text[i] != 0x7F ? text[i] : (u8)'.';
        buffer[written] = byte;
        written++;
    }

    if (length > NYA_WATCH_STRING_MAX && written + 4 < capacity) {
        nya_memcpy(&buffer[written], "...", 3);
        written += 3;
    }

    if (written + 1 < capacity) buffer[written++] = '"';

    buffer[written] = '\0';
    return written;
}

u32 nya_watch_value_format(NYA_WatchType type, u32 size, const void* address, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 1);
    nya_assert(type < NYA_WATCH_TYPE_COUNT);

    buffer[0] = '\0';

    if (address == nullptr) return _nya_watch_write(buffer, capacity, "<nowhere>");

    switch (type) {
        case NYA_WATCH_TYPE_SIGNED: {
            const s128 value = _nya_watch_read_signed(address, size);

            // Negated as an unsigned, because the most negative value has no positive counterpart.
            const u128 magnitude = value < 0 ? ~(u128)value + 1 : (u128)value;

            return _nya_watch_format_integer(magnitude, value < 0, buffer, capacity);
        }

        case NYA_WATCH_TYPE_UNSIGNED: return _nya_watch_format_integer(_nya_watch_read_unsigned(address, size), false, buffer, capacity);

        // %Lg rather than %Lf: a crash report is read for a wrong number's order of magnitude, and a value too small or large to see is exactly the kind that is wrong.
        case NYA_WATCH_TYPE_FLOAT: return _nya_watch_write(buffer, capacity, "%Lg", _nya_watch_read_float(address, size));

        case NYA_WATCH_TYPE_BOOL: return _nya_watch_write(buffer, capacity, "%s", _nya_watch_read_unsigned(address, size) != 0 ? "true" : "false");

        case NYA_WATCH_TYPE_CHAR: {
            const u8 value = (u8)_nya_watch_read_unsigned(address, 1);

            if (value < 0x20 || value == 0x7F) return _nya_watch_write(buffer, capacity, "'\\x%02x'", value);

            return _nya_watch_write(buffer, capacity, "'%c' (%u)", (char)value, value);
        }

        case NYA_WATCH_TYPE_CSTRING: {
            const char* text = nullptr;
            nya_memcpy(&text, address, sizeof(text));

            if (text == nullptr) return _nya_watch_write(buffer, capacity, "nullptr");

            /* The one read this function makes through a caller-given pointer, and the one that can fault: a pointer in the null page is refused (a member off a null struct); a dangling one hits the base_logging.c reentrancy guard. */
            if ((u64)(uintptr_t)text < _NYA_WATCH_NULL_PAGE) return _nya_watch_write(buffer, capacity, "0x%llx (unreadable)", (unsigned long long)(uintptr_t)text);

            u64 length = 0;
            while (length < NYA_WATCH_STRING_MAX + 1 && text[length] != '\0') length++;

            return _nya_watch_format_text((const u8*)text, length, buffer, capacity);
        }

        case NYA_WATCH_TYPE_STRING: {
            const NYA_String* string = nullptr;

            // the pointer's own width, spelled out: sizeof over a pointer to a struct reads as a mistake.
            nya_memcpy(&string, address, sizeof(const NYA_String*));

            if (string == nullptr) return _nya_watch_write(buffer, capacity, "nullptr");
            if ((u64)(uintptr_t)string < _NYA_WATCH_NULL_PAGE) return _nya_watch_write(buffer, capacity, "0x%llx (unreadable)", (unsigned long long)(uintptr_t)string);
            if (string->items == nullptr) return _nya_watch_write(buffer, capacity, "\"\" (empty)");

            u32 written  = _nya_watch_format_text(string->items, string->length, buffer, capacity);
            written     += _nya_watch_write(&buffer[written], capacity - written, " (" FMTu64 " bytes)", string->length);

            return written;
        }

        case NYA_WATCH_TYPE_POINTER: {
            const void* pointer = nullptr;
            nya_memcpy(&pointer, address, sizeof(pointer));

            if (pointer == nullptr) return _nya_watch_write(buffer, capacity, "nullptr");

            return _nya_watch_write(buffer, capacity, "0x%llx", (unsigned long long)(uintptr_t)pointer);
        }

        // Nothing is known about the bytes, so the address is worth printing: it is what a debugger on the core dump needs, and printing the bytes would be a guess.
        case NYA_WATCH_TYPE_OPAQUE: return _nya_watch_write(buffer, capacity, "<" FMTu32 " bytes at 0x%llx>", size, (unsigned long long)(uintptr_t)address);

        default: break;
    }
    static_assert(NYA_WATCH_TYPE_COUNT == 9, "Unhandled NYA_WatchType enum value.");

    return _nya_watch_write(buffer, capacity, "<unknown>");
}

// THE RING

u32 nya_watch_frame_begin(void) {
    return _nya_watch_depth;
}

void nya_watch_frame_end(u32 frame) {
    // A mark from a frame that has already unwound: nothing to take back, and nothing to complain about, because this runs while the stack is coming apart.
    if (frame >= _nya_watch_depth) return;

    _nya_watch_depth = frame;

    // Everything below the new depth was overwritten and is not coming back, so the readable range closes rather than exposing slots that now hold a newer frame's locals.
    if (_nya_watch_lost > _nya_watch_depth) _nya_watch_lost = _nya_watch_depth;
}

void nya_watch_record(
    u32              frame,
    NYA_ConstCString function,
    NYA_ConstCString name,
    NYA_ConstCString type_name,
    NYA_WatchType    type,
    u32              size,
    const void*      address
) {
    nya_assert(function != nullptr);
    nya_assert(name != nullptr);
    nya_assert(type_name != nullptr);

    // The slot about to be written still holds a live entry: say so before it goes, or the walk would read it back as belonging to the frame that has since taken the slot.
    if (_nya_watch_depth >= NYA_WATCH_RING_MAX) {
        const u32 oldest_kept = _nya_watch_depth - NYA_WATCH_RING_MAX + 1;
        if (oldest_kept > _nya_watch_lost) _nya_watch_lost = oldest_kept;
    }

    _nya_watch_ring[_nya_watch_depth % NYA_WATCH_RING_MAX] = (NYA_WatchEntry){
        .frame     = frame,
        .function  = function,
        .name      = name,
        .type_name = type_name,
        .address   = address,
        .type      = type,
        .size      = size,
    };

    _nya_watch_depth++;
}

u32 nya_watch_count(void) {
    return _nya_watch_depth - _nya_watch_lost;
}

const NYA_WatchEntry* nya_watch_at(u32 index) {
    if (index >= nya_watch_count()) return nullptr;

    return &_nya_watch_ring[(_nya_watch_lost + index) % NYA_WATCH_RING_MAX];
}

u32 nya_watch_dropped(void) {
    return _nya_watch_lost;
}
