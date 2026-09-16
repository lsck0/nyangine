/**
 * @file base_compress.h
 *
 * Block compression, wrapping LZ4. The whole module is three functions:
 *
 * - `nya_compress_bound` — the largest `nya_compress` could possibly write, so a caller can size a buffer
 *   before it has anything to measure.
 * - `nya_compress` — compress a block, reporting how many bytes it actually took.
 * - `nya_decompress` — expand one back, given the size it had before.
 *
 * ```c
 * u64 capacity   = nya_compress_bound(source_size);
 * u8* compressed = nya_arena_alloc(arena, capacity);
 * u64 written    = nya_compress(source, source_size, compressed, capacity);
 *
 * // Zero means it did not fit or did not help, and the caller stores the block verbatim instead.
 * if (written == 0 || written >= source_size) { ... }
 *
 * // The far side needs the original size; nothing in the compressed bytes carries it.
 * if (!nya_decompress(compressed, written, out, source_size)) { ... }
 * ```
 *
 * ⚠ **A compressed block does not describe itself.** There is no header, no magic and no length — the
 * output is exactly the codec's bytes. Whoever stores one has to store `source_size` beside it, because
 * `nya_decompress` cannot work it out and will not guess. That is deliberate: the callers here already
 * have somewhere to put it (a blob header, a save-file record), and eight bytes of framing per block is
 * worth more than the convenience.
 *
 * ⚠ **Malformed input is an operating error, not a programmer error.** `nya_decompress` returns false
 * rather than asserting, because the bytes can come from a file on disk or a peer on the network. The
 * bound and capacity checks on the way in are the opposite: those are the caller's contract and assert.
 *
 * LZ4 rather than anything stronger, because the decompression side is what runs in the shipped program:
 * it is memcpy-speed on the way out, which is the half that has to be free. Compression is slower and
 * happens once, at build time.
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The largest number of bytes `nya_compress` could write for an input of `size_bytes`.
 *
 * Slightly larger than the input, because incompressible data still has to be representable. Zero when
 * the input is empty or too large for the codec.
 * */
NYA_API u64 nya_compress_bound(u64 size_bytes) __attr_no_discard;

/**
 * Compresses `source_size_bytes` from `source` into `out`. Returns the bytes written, or zero.
 *
 * Zero means it would not fit in `out_capacity_bytes`, which for a buffer sized by `nya_compress_bound`
 * means the input was empty or too large. It is not an error and needs no error type: the caller's
 * answer is the same either way, which is to store the block uncompressed.
 * */
NYA_API u64 nya_compress(const void* source, u64 source_size_bytes, void* out, u64 out_capacity_bytes) __attr_no_discard;

/**
 * Expands `source_size_bytes` from `source` into exactly `out_size_bytes` at `out`.
 *
 * False when the input is malformed, truncated, or does not expand to exactly `out_size_bytes` — all of
 * which are the same answer to a caller: do not trust this block. `out` may have been partly written.
 * */
NYA_API b8 nya_decompress(const void* source, u64 source_size_bytes, void* out, u64 out_size_bytes) __attr_no_discard;
