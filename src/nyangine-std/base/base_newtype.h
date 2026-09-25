/**
 * @file base_newtype.h
 *
 * Parsed newtypes: a string or an integer that only exists once it has been checked, wrapped in a type
 * of its own so the compiler refuses to mix it with a bare string, a bare integer, or a sibling newtype.
 *
 * A one-field struct is the only newtype C has, and it is free at runtime; base_clock_instant.h makes the
 * same argument for NYA_Instant. What that idiom is missing is the parse: the moment where untrusted text
 * becomes a value the rest of the program may trust without checking again. This file is one macro for
 * that pattern, so `Email`, `Username` and `UserId` are a line of definition each instead of a bare
 * `char*` passed around and re-validated at every call that cares.
 *
 * ```c
 * NYA_Email email = { 0 };
 * if (!nya_email_from_string("user@example.com", &email).ok) return;   // refused, with a reason
 * printf("%s\n", nya_email_cstring(&email));                            // read the bytes back
 * ```
 *
 * ── what the macro gives a type ──
 *
 *   NYA_NEWTYPE_STRING(Type, prefix, capacity, validate)   a validated string, stored inline
 *   NYA_NEWTYPE_U64(Type, prefix, validate)                a validated u64 id, parsed from digits
 *
 * Each expands to a distinct one-field struct plus three functions named off `prefix`:
 *
 *   prefix##_from_string   NYA_ConstCString -> Type, or NYA_ERROR_INVALID_ARGUMENT and why. The parse.
 *   prefix##_cstring       Type -> the bytes back           (string newtypes)
 *   prefix##_value         Type -> the u64 back             (u64 newtypes)
 *   prefix##_equals        Type, Type -> b8
 *
 * ── the validation seam ──
 *
 * The definer supplies `validate`, so the macro knows the shape of a value without knowing what makes one
 * valid. For a string it is `b8 validate(const u8* bytes, u32 length)`, run after the length bound and
 * before the value is constructed. For a u64 it is `b8 validate(u64 value)`, run after the digits parse.
 * Return false and the parse fails; the macro never repairs input.
 *
 * ── bounded, and no allocation ──
 *
 * A string newtype stores its bytes inline in a fixed `capacity` buffer and keeps them NUL terminated, so
 * a value is copyable, comparable and owns nothing. `_from_string` reads at most `capacity` bytes of its
 * input and refuses anything that fills the buffer, so there is no unbounded copy and no arena to thread
 * through. Pick a `capacity` that fits the longest value the type admits plus its NUL.
 *
 * ── compile-time distinctness ──
 *
 * Because each `Type` is its own `struct`, `NYA_Email` and `NYA_Username` are not assignable to each
 * other, and neither is assignable to a `char*`: passing one where the other is expected is a type error
 * the compiler catches, not a runtime check that can be forgotten. The distinctness is the point of the
 * struct; the test suite exercises it by round-tripping each type, and cannot assert it directly because
 * the mismatch it guards against would not compile.
 * */
#pragma once

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_types.h"

// MACROS

// NOLINTBEGIN(bugprone-macro-parentheses): the type and declarator parameters (Type, prefix, capacity)
// name a type and paste identifiers, neither of which can be parenthesized.

/**
 * A newtype over a validated, inline-stored string. See the file header for the contract.
 *
 * `capacity` bounds the stored bytes including the NUL; `validate(const u8* bytes, u32 length)` decides
 * what a well formed value is, and is called once the length is known to fit.
 * */
#define NYA_NEWTYPE_STRING(Type, prefix, capacity, validate)                                                                                         \
    typedef struct Type {                                                                                                                            \
        /** NUL terminated, `length` bytes long, never more than `capacity - 1`. */                                                                  \
        u8  bytes[capacity];                                                                                                                         \
        u32 length;                                                                                                                                  \
    } Type;                                                                                                                                          \
                                                                                                                                                     \
    /** Parses and validates `input`, or fails with the rule it broke. Reads at most `capacity` bytes. */                                            \
    __attr_allow_unused NYA_INTERNAL NYA_Error prefix##_from_string(NYA_ConstCString input, OUT Type* out) __attr_no_discard;                        \
    __attr_allow_unused NYA_INTERNAL NYA_Error prefix##_from_string(NYA_ConstCString input, OUT Type* out) {                                         \
        nya_assert(input != nullptr, "%s_from_string needs an input.", #prefix);                                                                     \
        nya_assert(out != nullptr, "%s_from_string needs somewhere to write.", #prefix);                                                             \
                                                                                                                                                     \
        u64 length = 0;                                                                                                                              \
        while (length < (capacity) && input[length] != '\0') length++;                                                                               \
                                                                                                                                                     \
        if (length == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s cannot be empty.", #Type);                                                 \
        if (length >= (capacity))                                                                                                                    \
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s is too long: at least %llu bytes, the limit is %d.", #Type, (unsigned long long)length, \
                             (int)((capacity) - 1));                                                                                                  \
        if (!(validate)((const u8*)input, (u32)length)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s is not a valid %s.", input, #Type);         \
                                                                                                                                                     \
        Type result = { .length = (u32)length };                                                                                                     \
        nya_memcpy(result.bytes, input, length);                                                                                                     \
        result.bytes[length] = '\0';                                                                                                                 \
        *out                 = result;                                                                                                               \
        return NYA_OK;                                                                                                                               \
    }                                                                                                                                                \
                                                                                                                                                     \
    /** The validated bytes, NUL terminated. Borrowed from `self`; it owns them. */                                                                  \
    __attr_allow_unused NYA_INTERNAL NYA_ConstCString prefix##_cstring(const Type* self) __attr_no_discard;                                          \
    __attr_allow_unused NYA_INTERNAL NYA_ConstCString prefix##_cstring(const Type* self) {                                                           \
        nya_assert(self != nullptr, "%s_cstring needs a value.", #prefix);                                                                           \
        return (NYA_ConstCString)self->bytes;                                                                                                        \
    }                                                                                                                                                \
                                                                                                                                                     \
    /** Byte-for-byte equality. */                                                                                                                   \
    __attr_allow_unused NYA_INTERNAL b8 prefix##_equals(const Type* a, const Type* b) __attr_no_discard;                                             \
    __attr_allow_unused NYA_INTERNAL b8 prefix##_equals(const Type* a, const Type* b) {                                                              \
        nya_assert(a != nullptr && b != nullptr, "%s_equals needs two values.", #prefix);                                                            \
        return a->length == b->length && nya_memcmp(a->bytes, b->bytes, a->length) == 0;                                                             \
    }                                                                                                                                                \
    static_assert(true, "swallow the trailing semicolon")

/**
 * A newtype over a validated u64 id, parsed from a run of decimal digits. See the file header.
 *
 * `validate(u64 value)` decides which parsed numbers are ids of this type, and is called once the digits
 * are in range. The parse itself refuses an empty string, any non-digit, and a value that overflows u64.
 * */
#define NYA_NEWTYPE_U64(Type, prefix, validate)                                                                                                      \
    typedef struct Type {                                                                                                                            \
        u64 value;                                                                                                                                   \
    } Type;                                                                                                                                          \
                                                                                                                                                     \
    /** Parses `input` as decimal digits into an id, or fails: empty, a non-digit, overflow, or refused. */                                          \
    __attr_allow_unused NYA_INTERNAL NYA_Error prefix##_from_string(NYA_ConstCString input, OUT Type* out) __attr_no_discard;                        \
    __attr_allow_unused NYA_INTERNAL NYA_Error prefix##_from_string(NYA_ConstCString input, OUT Type* out) {                                         \
        nya_assert(input != nullptr, "%s_from_string needs an input.", #prefix);                                                                     \
        nya_assert(out != nullptr, "%s_from_string needs somewhere to write.", #prefix);                                                             \
                                                                                                                                                     \
        if (input[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s cannot be empty.", #Type);                                            \
                                                                                                                                                     \
        u64 value = 0;                                                                                                                               \
        for (const char* cursor = input; *cursor != '\0'; cursor++) {                                                                                \
            if (*cursor < '0' || *cursor > '9')                                                                                                      \
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s takes decimal digits only, not '%s'.", #Type, input);                               \
                                                                                                                                                     \
            u64 digit = (u64)(*cursor - '0');                                                                                                        \
            if (value > (U64_MAX - digit) / 10)                                                                                                      \
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s overflows a u64: '%s'.", #Type, input);                                             \
            value = value * 10 + digit;                                                                                                              \
        }                                                                                                                                            \
                                                                                                                                                     \
        if (!(validate)(value)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%llu is not a valid %s.", (unsigned long long)value, #Type);           \
                                                                                                                                                     \
        *out = (Type){ .value = value };                                                                                                             \
        return NYA_OK;                                                                                                                               \
    }                                                                                                                                                \
                                                                                                                                                     \
    /** The validated number. */                                                                                                                     \
    __attr_allow_unused NYA_INTERNAL u64 prefix##_value(Type self) __attr_no_discard;                                                                \
    __attr_allow_unused NYA_INTERNAL u64 prefix##_value(Type self) { return self.value; }                                                            \
                                                                                                                                                     \
    /** Equality of the underlying ids. */                                                                                                           \
    __attr_allow_unused NYA_INTERNAL b8 prefix##_equals(Type a, Type b) __attr_no_discard;                                                           \
    __attr_allow_unused NYA_INTERNAL b8 prefix##_equals(Type a, Type b) { return a.value == b.value; }                                               \
    static_assert(true, "swallow the trailing semicolon")

// NOLINTEND(bugprone-macro-parentheses)

/* THE THREE PARSED TYPES: the types TODO.md's SO section names, each the macro above plus one predicate function that is the whole definition of what the type admits. */

/** RFC 5321's ceiling on an address, plus the NUL. Anything longer is refused before it is looked at. */
#define NYA_EMAIL_CAPACITY 255

/** A username is 3 to 32 characters; 32 plus the NUL. */
#define NYA_USERNAME_CAPACITY   33
#define NYA_USERNAME_MIN_LENGTH 3

/**
 * A practical syntactic check, not RFC 5322: a non-empty local part, exactly one '@', and a dotted domain
 * of letter-digit-hyphen labels ending in an all-letter TLD of two or more. It rejects what is plainly not
 * an address; it does not promise the mailbox exists. Deliverability is a different question, answered by
 * sending mail, not by a grammar.
 * */
__attr_allow_unused NYA_INTERNAL b8 nya_email_is_valid(const u8* bytes, u32 length) __attr_no_discard;
__attr_allow_unused NYA_INTERNAL b8 nya_email_is_valid(const u8* bytes, u32 length) {
    // exactly one '@', with something on either side.
    u32 at = 0;
    u32 at_count = 0;
    for (u32 i = 0; i < length; i++) {
        if (bytes[i] == '@') {
            at_count++;
            at = i;
        }
    }
    if (at_count != 1) return false;

    const u32 local_length  = at;
    const u32 domain_start  = at + 1;
    const u32 domain_length = length - domain_start;
    if (local_length == 0 || domain_length == 0) return false;

    // Local part: visible ASCII that is neither space nor control, with dots that neither lead, trail nor double; a sane subset of RFC 5321's atext, not the quoted-string escape hatch.
    if (bytes[0] == '.' || bytes[local_length - 1] == '.') return false;
    for (u32 i = 0; i < local_length; i++) {
        const u8 c = bytes[i];
        if (c <= ' ' || c >= 0x7F) return false;
        if (c == '.' && bytes[i + 1] == '.') return false;
    }

    // domain: dot-separated labels of letters, digits and interior hyphens, ending in an all-letter TLD.
    u32 label_length     = 0;
    u32 last_label_length = 0;
    b8  last_label_alpha  = true;
    for (u32 i = domain_start; i < length; i++) {
        const u8 c = bytes[i];
        if (c == '.') {
            if (label_length == 0) return false;             // empty label: leading, trailing or doubled dot.
            if (bytes[i - 1] == '-') return false;           // a label may not end in a hyphen.
            label_length = 0;
            continue;
        }

        const b8 is_alpha  = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const b8 is_digit  = c >= '0' && c <= '9';
        const b8 is_hyphen = c == '-';
        if (!is_alpha && !is_digit && !is_hyphen) return false;
        if (is_hyphen && label_length == 0) return false;    // a label may not begin with a hyphen.

        label_length++;
        last_label_length = label_length;
        last_label_alpha  = (label_length == 1) ? is_alpha : (last_label_alpha && is_alpha);
    }

    if (label_length == 0) return false;                     // a trailing dot.
    // the domain must have had at least one dot: a TLD of its own is not a domain we will act on.
    b8 has_dot = false;
    for (u32 i = domain_start; i < length; i++) has_dot |= bytes[i] == '.';
    if (!has_dot) return false;

    return last_label_alpha && last_label_length >= 2;
}

/**
 * A username is 3 to 32 characters of ASCII letters, digits, underscore and hyphen, beginning with a
 * letter or a digit. Deliberately narrow: no case folding, no Unicode, nothing that two different byte
 * strings can render the same way, because a username is an identity and a look-alike is an attack.
 * */
__attr_allow_unused NYA_INTERNAL b8 nya_username_is_valid(const u8* bytes, u32 length) __attr_no_discard;
__attr_allow_unused NYA_INTERNAL b8 nya_username_is_valid(const u8* bytes, u32 length) {
    if (length < NYA_USERNAME_MIN_LENGTH) return false;

    for (u32 i = 0; i < length; i++) {
        const u8 c        = bytes[i];
        const b8 is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const b8 is_digit = c >= '0' && c <= '9';
        const b8 is_extra = c == '_' || c == '-';

        if (i == 0 && !is_alpha && !is_digit) return false;  // must begin alphanumeric, not with punctuation.
        if (!is_alpha && !is_digit && !is_extra) return false;
    }

    return true;
}

/** A user id is any non-zero number: zero is the null id no row ever has. */
__attr_allow_unused NYA_INTERNAL b8 nya_user_id_is_valid(u64 value) __attr_no_discard;
__attr_allow_unused NYA_INTERNAL b8 nya_user_id_is_valid(u64 value) { return value != 0; }

NYA_NEWTYPE_STRING(NYA_Email, nya_email, NYA_EMAIL_CAPACITY, nya_email_is_valid);
NYA_NEWTYPE_STRING(NYA_Username, nya_username, NYA_USERNAME_CAPACITY, nya_username_is_valid);
NYA_NEWTYPE_U64(NYA_UserId, nya_user_id, nya_user_id_is_valid);
