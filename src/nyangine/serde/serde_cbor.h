/**
 * @file serde_cbor.h
 *
 * Just enough CBOR to read what WebAuthn hands a server, and nothing more.
 *
 * ```c
 * NYA_CborReader reader = { 0 };
 * nya_cbor_reader_init(&reader, data, size);
 *
 * u64 entries = 0;
 * if (!nya_cbor_read_map(&reader, &entries)) return refused;   // a definite-length map
 *
 * for (u64 index = 0; index < entries; index++) {
 *     s64 label = 0;
 *     if (!nya_cbor_read_int(&reader, &label)) return refused;  // COSE labels are integers
 *     // ... read or skip the value for this label
 * }
 * ```
 *
 * ── why this exists, and why it is this small ──
 *
 * WebAuthn's attestation object and its COSE public keys are CBOR (RFC 8949), and CBOR is the one
 * encoding on the wire that `serde` did not already have. Rather than vendor a general CBOR library —
 * a decoder for every tag, float and indefinite-length form is a large attack surface for a server to
 * expose to an authenticator's bytes — this reads only the shapes those two structures actually use:
 * unsigned and negative integers, byte and text strings, and definite-length maps and arrays. A
 * `skip` steps over any well-formed value so an unknown map entry (an attestation statement, a COSE
 * label this does not care about) does not stop the read.
 *
 * ── it is a reader, and it is defensive ──
 *
 * There is no encoder: a server verifies what a client sent, it does not produce CBOR. Every read is
 * bounds checked against the buffer it was given before a byte is touched, because the bytes are
 * untrusted input from across the network — a truncated or a hostile encoding must fail the read, not
 * walk off the end of the buffer. Indefinite-length items are refused outright rather than parsed, and
 * a nested structure is skipped only to a bounded depth, so neither is a way to exhaust the stack.
 *
 * A string read returns a pointer *into* the caller's buffer rather than a copy: the buffer outlives
 * the read, and a copy would be one this cannot size in advance.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/**
 * How deep a skip will follow a nested map or array before it gives up.
 *
 * A value nested past this is refused rather than followed, so a hostile encoding of a map inside a map
 * inside a map cannot drive the skip's recursion into the stack. The structures this reads are two
 * levels deep at most; sixteen is far past anything well-formed.
 * */
#define NYA_CBOR_MAX_DEPTH 16

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_CborReader NYA_CborReader;

/**
 * A cursor over a CBOR buffer, moving forward as items are read.
 *
 * Points at the caller's bytes and never owns them. `offset` is how far the read has got; every read
 * advances it, and no read ever moves it past `size`.
 * */
struct NYA_CborReader {
    const u8* data;
    u64       size;
    u64       offset;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/** A reader aimed at `size` bytes at `data`. A null buffer or a zero size is a reader every read fails. */
NYA_API NYA_CborReader nya_cbor_reader(const u8* data, u64 size) __attr_no_discard;

/**
 * Reads an unsigned integer (major type 0) and advances past it.
 *
 * False for anything that is not one, including a negative integer, and for an argument that runs off
 * the end of the buffer.
 * */
NYA_API b8 nya_cbor_read_u64(NYA_CborReader* reader, OUT u64* out_value) __attr_no_discard;

/**
 * Reads a signed integer — an unsigned one (major type 0) or a negative one (major type 1) — and
 * advances past it.
 *
 * The two are one type here because COSE labels and values are either: `kty` is a small positive
 * number, `alg` a negative one. A negative CBOR integer encodes `-1 - n`; this returns the signed value
 * that is. False for a magnitude that a signed 64-bit value cannot hold, so an overflow is a refused
 * read rather than a wrapped one.
 * */
NYA_API b8 nya_cbor_read_int(NYA_CborReader* reader, OUT s64* out_value) __attr_no_discard;

/**
 * Reads a byte string (major type 2), handing back a pointer into the buffer and its length.
 *
 * `out_bytes` points into the reader's own buffer and is valid for as long as that buffer is; nothing is
 * copied. False for anything that is not a byte string, an indefinite-length one, or a length that runs
 * past the end of the buffer.
 * */
NYA_API b8 nya_cbor_read_bytes(NYA_CborReader* reader, OUT const u8** out_bytes, OUT u64* out_size) __attr_no_discard;

/**
 * Reads a text string (major type 3), handing back a pointer into the buffer and its length.
 *
 * The same as nya_cbor_read_bytes but for the text major type, and it does not validate UTF-8 or append
 * a terminator: a caller comparing a short known key does so over the length this returns.
 * */
NYA_API b8 nya_cbor_read_text(NYA_CborReader* reader, OUT const u8** out_text, OUT u64* out_size) __attr_no_discard;

/**
 * Reads a definite-length map header (major type 5) and answers how many key/value pairs follow.
 *
 * The pairs themselves are read by the caller, a key then a value at a time. False for an array, for an
 * indefinite-length map, or for a count whose argument runs off the buffer.
 * */
NYA_API b8 nya_cbor_read_map(NYA_CborReader* reader, OUT u64* out_count) __attr_no_discard;

/**
 * Reads a definite-length array header (major type 4) and answers how many items follow.
 *
 * As nya_cbor_read_map, for the array major type.
 * */
NYA_API b8 nya_cbor_read_array(NYA_CborReader* reader, OUT u64* out_count) __attr_no_discard;

/**
 * Steps over one whole value — of any kind, nesting and all — without interpreting it.
 *
 * What lets a reader walk past a map entry it does not care about: read the key, skip the value. Follows
 * a nested map or array down to NYA_CBOR_MAX_DEPTH and no further, and refuses an indefinite-length item
 * rather than chasing its break, so a hostile encoding cannot make it loop or recurse without bound.
 * False when the value is malformed or would run off the buffer.
 * */
NYA_API b8 nya_cbor_skip(NYA_CborReader* reader) __attr_no_discard;
