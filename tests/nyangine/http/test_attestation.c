/**
 * The mirror attestation: an origin signs a manifest of who it is and what it serves, and a verifier
 * with the pinned origin key can tell a faithful mirror from a tampered one.
 *
 * All deterministic, no socket: the origin key is derived from a fixed seed — a throwaway pair made here
 * and never checked in — so the same run signs the same bytes, and the document is built and parsed as a
 * NYA_Object, which is what would cross the wire as JSON. The three cases the header promises: a
 * sign→verify round-trip passes, a manifest changed after signing fails, and the right signature under
 * the wrong key fails. Plus the bundle digest is stable and order-sensitive, and the served document
 * round-trips back through from_json into a manifest that still verifies.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** A fixed seed, so the origin key is the same every run. A throwaway pair for the test, not a secret to keep. */
static const NYA_CryptoKey32 SEED = { .bytes = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                                  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20 } };

/** A second seed, for the wrong-key case. */
static const NYA_CryptoKey32 OTHER_SEED = { .bytes = { 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
                                                        0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF } };

/** A manifest over a made-up onion and a made-up content digest. */
static NYA_HttpAttestationManifest sample_manifest(void) {
    NYA_HttpAttestationManifest manifest = { .issued_at_s = 1700000000ULL };

    (void)snprintf(manifest.origin, sizeof(manifest.origin), "http://expyuzz4wqqyqhjn.onion");

    // A content digest with recognisable bytes, so a flip in a test is obvious.
    for (u64 index = 0; index < NYA_CRYPTO_SHA256_BYTES; index++) manifest.content.bytes[index] = (u8)(index + 1);

    return manifest;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_attestation");
    defer      nya_arena_destroy(arena);

    NYA_CryptoSignKeyPair origin = { 0 };
    NYA_CryptoSignKeyPair other  = { 0 };
    nya_crypto_sign_key_pair_from_seed(&SEED, &origin);
    nya_crypto_sign_key_pair_from_seed(&OTHER_SEED, &other);

    defer nya_crypto_sign_key_pair_destroy(&origin);
    defer nya_crypto_sign_key_pair_destroy(&other);

    // TEST: sign → verify round-trip.
    {
        NYA_HttpAttestationManifest manifest = sample_manifest();

        NYA_CryptoSignature signature = { 0 };
        NYA_EXPECT(nya_http_attestation_sign(&origin.secret_key, &manifest, &signature), "while signing");

        nya_check(nya_http_attestation_verify(&origin.public_key, &manifest, &signature), "a signature verifies against the origin key");
    }

    // TEST: a manifest changed after signing does not verify — neither the origin nor the content.
    {
        NYA_HttpAttestationManifest manifest = sample_manifest();

        NYA_CryptoSignature signature = { 0 };
        NYA_EXPECT(nya_http_attestation_sign(&origin.secret_key, &manifest, &signature), "while signing");

        NYA_HttpAttestationManifest changed_content = manifest;
        changed_content.content.bytes[0] ^= 0x01;
        nya_check(!nya_http_attestation_verify(&origin.public_key, &changed_content, &signature), "a changed content digest is rejected");

        NYA_HttpAttestationManifest changed_origin = manifest;
        (void)snprintf(changed_origin.origin, sizeof(changed_origin.origin), "http://evil7777777777777.onion");
        nya_check(!nya_http_attestation_verify(&origin.public_key, &changed_origin, &signature), "a changed origin is rejected");

        NYA_HttpAttestationManifest changed_time = manifest;
        changed_time.issued_at_s += 1;
        nya_check(!nya_http_attestation_verify(&origin.public_key, &changed_time, &signature), "a changed timestamp is rejected");
    }

    // TEST: a good signature under the wrong key is rejected — the pin is what makes this mean anything.
    {
        NYA_HttpAttestationManifest manifest = sample_manifest();

        NYA_CryptoSignature signature = { 0 };
        NYA_EXPECT(nya_http_attestation_sign(&origin.secret_key, &manifest, &signature), "while signing");

        nya_check(!nya_http_attestation_verify(&other.public_key, &manifest, &signature), "the origin's signature does not verify under another key");
    }

    // TEST: the bundle digest is stable, and it changes when a byte or the order changes.
    {
        const u8 index_html[] = "<!doctype html><title>home</title>";
        const u8 app_css[]    = "body{color:#111}";
        const u8 app_js[]     = "console.log('hi')";

        NYA_HttpAttestationFile files[] = {
            { .path = "/",        .data = index_html, .size = sizeof(index_html) - 1 },
            { .path = "/app.css", .data = app_css,    .size = sizeof(app_css) - 1    },
            { .path = "/app.js",  .data = app_js,     .size = sizeof(app_js) - 1     },
        };

        NYA_CryptoSha256Digest a = { 0 };
        NYA_CryptoSha256Digest b = { 0 };
        nya_http_attestation_bundle_digest(files, nya_carray_length(files), &a);
        nya_http_attestation_bundle_digest(files, nya_carray_length(files), &b);
        nya_check(nya_memcmp(a.bytes, b.bytes, NYA_CRYPTO_SHA256_BYTES) == 0, "the same bundle folds to the same digest");

        // Reorder two files: a different bundle, so a different digest.
        NYA_HttpAttestationFile reordered[] = { files[1], files[0], files[2] };
        NYA_CryptoSha256Digest  c           = { 0 };
        nya_http_attestation_bundle_digest(reordered, nya_carray_length(reordered), &c);
        nya_check(nya_memcmp(a.bytes, c.bytes, NYA_CRYPTO_SHA256_BYTES) != 0, "reordering the bundle changes the digest");

        // Change one byte of one file: a different digest, which is a mirror serving something else.
        const u8                tampered_js[] = "console.log('HI')";
        NYA_HttpAttestationFile changed[]     = { files[0], files[1], { .path = "/app.js", .data = tampered_js, .size = sizeof(tampered_js) - 1 } };
        NYA_CryptoSha256Digest  d             = { 0 };
        nya_http_attestation_bundle_digest(changed, nya_carray_length(changed), &d);
        nya_check(nya_memcmp(a.bytes, d.bytes, NYA_CRYPTO_SHA256_BYTES) != 0, "changing a byte changes the digest");
    }

    // TEST: the served document round-trips — to_json then from_json yields a manifest that still verifies,
    //       and the parsed public key is the origin's, which is what a verifier compares to its pin.
    {
        NYA_HttpAttestationManifest manifest = sample_manifest();

        NYA_CryptoSignature signature = { 0 };
        NYA_EXPECT(nya_http_attestation_sign(&origin.secret_key, &manifest, &signature), "while signing");

        NYA_Object* document = nullptr;
        NYA_EXPECT(nya_http_attestation_to_json(arena, &manifest, &origin.public_key, &signature, &document), "while rendering the document");

        // Render to JSON and parse it back, which is the trip the wire makes, not just a struct copy.
        NYA_String* json = nya_serialize(arena, document, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
        nya_check(json != nullptr, "the document serializes to JSON");

        NYA_Object* parsed = nullptr;
        NYA_EXPECT(nya_deserialize(arena, json->items, json->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &parsed), "while parsing the document back");

        NYA_HttpAttestationManifest got_manifest = { 0 };
        NYA_CryptoSignPublicKey     got_key       = { 0 };
        NYA_CryptoSignature         got_signature = { 0 };
        NYA_EXPECT(nya_http_attestation_from_json(parsed, &got_manifest, &got_key, &got_signature), "while reading the document");

        nya_check(strcmp(got_manifest.origin, manifest.origin) == 0, "the parsed origin matches");
        nya_check(got_manifest.issued_at_s == manifest.issued_at_s, "the parsed timestamp matches");
        nya_check(nya_memcmp(got_manifest.content.bytes, manifest.content.bytes, NYA_CRYPTO_SHA256_BYTES) == 0, "the parsed content digest matches");
        nya_check(nya_memcmp(got_key.bytes, origin.public_key.bytes, NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES) == 0, "the parsed key is the origin's, which a verifier compares to its pin");
        nya_check(nya_http_attestation_verify(&got_key, &got_manifest, &got_signature), "the round-tripped manifest still verifies");
    }

    // TEST: from_json refuses a document with a field of the wrong length rather than half-reading it.
    {
        NYA_Object* broken = nya_object_create(arena);
        nya_object_add(broken, "origin", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"http://x.onion" });
        nya_object_add(broken, "issued_at_s", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1700000000 });
        nya_object_add(broken, "content_sha256", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"AAAA" });      // 3 bytes, not 32
        nya_object_add(broken, "public_key", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"AAAA" });
        nya_object_add(broken, "signature", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"AAAA" });

        NYA_HttpAttestationManifest m = { 0 };
        NYA_CryptoSignPublicKey     k = { 0 };
        NYA_CryptoSignature         s = { 0 };
        nya_check(!nya_http_attestation_from_json(broken, &m, &k, &s).ok, "a field of the wrong length is refused");
    }

    printf("test_attestation: %u assertion(s) failed.\n", nya_check_failures());

    return nya_check_failures() == 0 ? 0 : 1;
}
