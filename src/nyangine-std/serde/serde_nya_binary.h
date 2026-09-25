/**
 * @file serde_nya_binary.h
 *
 * The same NYA_Object as the text `.nya`, as compact bytes: what two nyangine programs send each other
 * over HTTP as `application/nya-binary`, and what a reflected DTO travels as.
 *
 * Overview:
 *   nya_serde_nya_binary_encode         an object to bytes, optionally tied to a reflected type
 *   nya_serde_nya_binary_decode         bytes to an object, refusing a layout it was not built for
 *   nya_serde_nya_binary_serialize      encode through the serde dispatch, untyped
 *   nya_serde_nya_binary_deserialize    decode through the serde dispatch, untyped
 *
 * ```c
 * NYA_String* bytes = nullptr;
 * NYA_TRY(nya_serde_nya_binary_encode(arena, document, nya_reflect_of(NYA_HttpAccountingDto), &bytes));
 *
 * NYA_Object* back = nullptr;
 * NYA_TRY(nya_serde_nya_binary_decode(arena, bytes->items, bytes->length, nya_reflect_of(NYA_HttpAccountingDto), &back));
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * THE FORMAT
 * ─────────────────────────────────────────────────────────
 *
 * Every integer is little endian and of the width written here. A 16 byte header, then the root
 * object's body:
 *
 * ```
 * offset  bytes  field
 *      0      4  magic 89 6E 79 61, "\x89nya"
 *      4      2  version, NYA_SERDE_NYA_BINARY_VERSION
 *      6      2  flags: bit 0 is TYPED, every other bit must be zero
 *      8      8  layout hash of the type the document describes; zero, and required to be, when untyped
 *     16         the root object's body
 * ```
 *
 * A value is a one byte tag and a payload. The tags are fixed forever and are not NYA_Type's numbers,
 * so a member added to that enum does not move them:
 *
 * ```
 * 00 null        no payload
 * 01..05         b8 b16 b32 b64 b128, one byte, 0 or 1 and nothing else
 * 06..0A         u8 u16 u32 u64 u128, 1 2 4 8 16 bytes
 * 0B..0F         s8 s16 s32 s64 s128, the same widths, two's complement
 * 10..13         f16 f32 f64 f128, 2 4 8 bytes of IEEE 754, and the 10 bytes of an x87 extended
 * 14 char        one byte
 * 15 string      u32 length, then the bytes, none of them zero
 * 16 object      u32 member count, then per member: u8 key length, the key's bytes, a value
 * 17 array       u32 element count, then, when not empty, an element tag and the elements
 * 18 any         only as an array's element tag: every element carries its own tag
 * ```
 *
 * An array whose elements share a type other than null names it once and writes bare payloads, the
 * way the text form writes `u32[] [1, 2]`. Otherwise its element tag is `any`.
 *
 * ─────────────────────────────────────────────────────────
 * ONE ENCODING PER DOCUMENT
 * ─────────────────────────────────────────────────────────
 *
 * The encoder writes exactly one byte sequence for a given object and the decoder accepts only that
 * one, so encode(decode(bytes)) == bytes for every input that decodes. It is what the fuzz target
 * asserts, and it is why these are refused rather than tolerated:
 *
 * - **Keys out of order.** Members are written sorted by strcmp. Unsorted input is refused.
 * - **Duplicate keys.** Refused, not "last one wins". Two readers that resolve a duplicate differently
 *   read two different documents from one body, which is how a request gets past a check that looked
 *   at the other one. Sorted keys make this a comparison with the previous key, not a lookup.
 * - **`any` where one type would do.** An `any` array whose elements all share a non-null type.
 * - **A boolean byte other than 0 or 1.**
 * - **A zero byte inside a key or a string.** The object holds C strings, and a reader that stopped at
 *   the zero would see a shorter string than the length claims.
 * - **Unknown tags, unknown flag bits, other versions, trailing bytes and truncation**, always.
 *
 * ─────────────────────────────────────────────────────────
 * TYPED AND UNTYPED DOCUMENTS
 * ─────────────────────────────────────────────────────────
 *
 * A document encoded against a reflected type sets TYPED and carries nya_reflect_layout_hash of it;
 * decoding it takes the same type and refuses a different hash with both numbers and the type's name,
 * rather than filling a struct whose fields moved. The encoder refuses an object that does not fit
 * the type (nya_reflect_check), and so does the decoder, so a typed document never claims a shape it
 * does not have.
 *
 * An untyped document clears TYPED and its hash must be zero. The flag, not the zero, is what says
 * so: zero is a hash like any other, and reading it as "no type" would make one real type
 * indistinguishable from none. Each side refuses the other: an untyped reader is not handed a DTO it
 * has no layout to check, and a typed reader is not handed a document that never claimed its type.
 *
 * ─────────────────────────────────────────────────────────
 * BOUNDS
 * ─────────────────────────────────────────────────────────
 *
 * Every length and count is checked against the bytes left before anything is allocated, so a length
 * prefix claiming four billion elements costs nothing but the error. Every value costs at least one
 * input byte, which is why null is not a shared element type (an array of a million nulls would
 * otherwise be ten bytes long). Recursion is bounded by NYA_SERDE_NYA_BINARY_DEPTH_MAX.
 *
 * Fixed widths rather than varints: counts are few per document, a fixed width is one bounds check
 * rather than a loop with its own overflow cases, and the bytes saved are a handful per document.
 *
 * No checksum, unlike the text form: a body has TCP (and later TLS) under it, a file is written through
 * nya_file_write_atomic, and every byte is structurally checked here anyway. A checksum would catch
 * nothing that is not already refused, and nothing at all against someone who can recompute it.
 * */
#pragma once

#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_reflection.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/serde/serde_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/**
 * The first four bytes. 0x89 first because no text format may start with it, so the sniffer is never
 * in doubt, and because a transport that strips the high bit mangles it visibly; PNG's reasoning.
 * */
#define NYA_SERDE_NYA_BINARY_MAGIC       "\x89nya"
#define NYA_SERDE_NYA_BINARY_MAGIC_BYTES 4

#define NYA_SERDE_NYA_BINARY_VERSION 1

#define NYA_SERDE_NYA_BINARY_HEADER_BYTES 16

/** The one flag the header defines. */
#define NYA_SERDE_NYA_BINARY_FLAG_TYPED 0x0001

/**
 * Largest document either side handles, header included.
 *
 * The biggest thing sent today is an HTTP body, capped at NYA_HTTP_MAX_BODY_BYTES (8 KB); a save record
 * is a few kilobytes. Four megabytes is two orders of magnitude of room and still bounds what a file
 * handed to nya_serde_load_file can make the decoder do.
 * */
#ifndef NYA_SERDE_NYA_BINARY_SIZE_MAX
#define NYA_SERDE_NYA_BINARY_SIZE_MAX (4ULL * 1024 * 1024)
#endif

/**
 * Values in one document, every nesting level counted. A decoded value costs about a hundred bytes of
 * arena (the NYA_Value and its share of the object's table), so this caps a hostile document at
 * roughly 26 MB of decode memory however it is shaped. Only a file can get near it: every value costs
 * an input byte, so an 8 KB HTTP body holds at most eight thousand.
 * */
#ifndef NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX
#define NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX (1U << 18)
#endif

/** Nesting of objects and arrays, the root object being depth one. The text form's bound, so either reads what the other wrote. */
#ifndef NYA_SERDE_NYA_BINARY_DEPTH_MAX
#define NYA_SERDE_NYA_BINARY_DEPTH_MAX 128
#endif

/** Longest key, in bytes. A key is a field name, and its length prefix is one byte. */
#define NYA_SERDE_NYA_BINARY_KEY_BYTES_MAX 255

// ───────────────────────────────────── FUNCTIONS AND MACROS ─────────────────────────────────────

/**
 * Encodes `object` into `out_bytes`, allocated from `arena`.
 *
 * With `type`, the document is TYPED and carries its layout hash, and an object that does not fit
 * the type is refused. Without it the document is untyped. Refuses, rather than truncates, anything
 * past the bounds above, and a value of a type the format has no tag for (pointers, wide strings).
 * */
NYA_API NYA_Error nya_serde_nya_binary_encode(NYA_Arena* arena, const NYA_Object* object, const NYA_TypeReflection* type, OUT NYA_String** out_bytes)
    __attr_no_discard;

/**
 * Decodes `size` bytes into `out_object`, allocated from `arena`. Every error names the byte offset.
 *
 * `type` is what the caller expects: null for an untyped document, or the type whose layout hash the
 * header must carry. Never allocates in proportion to a length it has not checked against the input.
 * */
NYA_API NYA_Error nya_serde_nya_binary_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_TypeReflection* type, OUT NYA_Object** out_object)
    __attr_no_discard;

/**
 * nya_serde_nya_binary_encode without a type, in the shape the dispatch in serde.h wants. The flags
 * mean nothing to a binary document and are ignored. Null, with the reason logged, where encode fails.
 * */
NYA_API NYA_String* nya_serde_nya_binary_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/** nya_serde_nya_binary_decode without a type. The flags are ignored. */
NYA_API NYA_Error nya_serde_nya_binary_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;
