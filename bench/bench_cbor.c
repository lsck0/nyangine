/**
 * The CBOR reader, which a server runs over every attestation object and COSE key an authenticator sends.
 *
 * nya_cbor_* is a bounds-checked reader with no allocation: it walks the caller's bytes, handing back
 * pointers into them. A WebAuthn registration parses a COSE public key — a small definite-length map of
 * integer labels — so this measures the two shapes that path uses: the label-and-skip walk over that
 * map, and a single skip over the whole value the way a parser steps past an entry it does not want.
 * The buffer is a real ES256 COSE key: kty, alg, curve, and the two 32-byte coordinates.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Appends `count` bytes of a fixed pattern, standing in for a coordinate's bytes. */
static u64 fill_pattern(u8* out, u64 at, u64 count) {
    for (u64 i = 0; i < count; i++) out[at + i] = (u8)(0x40 + (i & 0x1F));
    return at + count;
}

/** A definite-length COSE EC2 public key: {1:2, 3:-7, -1:1, -2:x(32), -3:y(32)}. */
static u64 build_cose_key(u8* out) {
    u64 at = 0;

    out[at++] = 0xA5;              // map, 5 pairs
    out[at++] = 0x01; out[at++] = 0x02;   // kty (1): EC2 (2)
    out[at++] = 0x03; out[at++] = 0x26;   // alg (3): ES256 (-7)
    out[at++] = 0x20; out[at++] = 0x01;   // crv (-1): P-256 (1)
    out[at++] = 0x21; out[at++] = 0x58; out[at++] = 0x20;   // x (-2): byte string, 32 bytes
    at        = fill_pattern(out, at, 32);
    out[at++] = 0x22; out[at++] = 0x58; out[at++] = 0x20;   // y (-3): byte string, 32 bytes
    at        = fill_pattern(out, at, 32);

    return at;
}

s32 main(void) {
    u8  buffer[128] = { 0 };
    u64 size        = build_cose_key(buffer);

    nya_bench_begin("CBOR reader (per COSE key)");

    // The registration walk: a map header, then a label and a skipped value per entry — how the parser reads a COSE key without a general decoder.
    nya_bench("walk a COSE key map", 1, {
        NYA_CborReader reader  = nya_cbor_reader(buffer, size);
        u64            entries = 0;
        b8             ok      = nya_cbor_read_map(&reader, &entries);

        for (u64 index = 0; ok && index < entries; index++) {
            s64 label = 0;
            ok = ok && nya_cbor_read_int(&reader, &label);
            ok = ok && nya_cbor_skip(&reader);
        }
        nya_bench_keep(ok ? reader.offset : 0);
    });

    // The same bytes stepped over in one call: what walking past an entry a parser does not care about costs.
    nya_bench("skip the whole value", 1, {
        NYA_CborReader reader = nya_cbor_reader(buffer, size);
        b8             ok     = nya_cbor_skip(&reader);
        nya_bench_keep(ok ? reader.offset : 0);
    });

    return nya_bench_end();
}
