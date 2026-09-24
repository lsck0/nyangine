/**
 * @file base_compress.h
 *
 * Block compression, wrapping LZ4.
 *
 * - `nya_compress_bound`: the most `nya_compress` can write, to size a buffer up front.
 * - `nya_compress`: compress a block, returning the bytes written.
 * - `nya_decompress`: expand a block, given its original size.
 *
 * ```c
 * u64 capacity   = nya_compress_bound(source_size);
 * u8* compressed = nya_arena_alloc(arena, capacity);
 * u64 written    = nya_compress(source, source_size, compressed, capacity);
 *
 * // zero means it did not fit or did not help: store the block verbatim.
 * if (written == 0 || written >= source_size) { ... }
 *
 * // the compressed bytes do not carry the original size.
 * if (!nya_decompress(compressed, written, out, source_size)) { ... }
 * ```
 *
 * A compressed block has no header, magic or length. Whoever stores one stores `source_size` beside
 * it. The callers already have a place for it (blob header, save record), so framing each block
 * would be eight wasted bytes.
 *
 * Malformed input is an operating error: `nya_decompress` returns false, since the bytes can come from
 * disk or a peer. Bound and capacity checks on the way in are the caller's contract and assert.
 *
 * LZ4 because decompression runs in the shipped program and is near memcpy speed. Compression is
 * slower and happens once, at build time.
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

// FUNCTIONS AND MACROS

/**
 * The most bytes `nya_compress` can write for `size_bytes` of input.
 *
 * Slightly larger than the input, since incompressible data must still fit. Zero when the input is
 * empty or too large for the codec.
 * */
NYA_API u64 nya_compress_bound(u64 size_bytes) __attr_no_discard;

/**
 * Compresses `source_size_bytes` from `source` into `out`. Returns the bytes written, or zero.
 *
 * Zero means it did not fit in `out_capacity_bytes`; with a buffer from `nya_compress_bound` that
 * means the input was empty or too large. Either way the caller stores the block uncompressed.
 * */
NYA_API u64 nya_compress(const void* source, u64 source_size_bytes, void* out, u64 out_capacity_bytes) __attr_no_discard;

/**
 * Expands `source_size_bytes` from `source` into exactly `out_size_bytes` at `out`.
 *
 * False when the input is malformed, truncated, or expands to a different size. `out` may be partly
 * written.
 * */
NYA_API b8 nya_decompress(const void* source, u64 source_size_bytes, void* out, u64 out_size_bytes) __attr_no_discard;
