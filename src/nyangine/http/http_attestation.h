/**
 * @file http_attestation.h
 *
 * A signed statement binding an origin to the bytes it serves, so a mirror can be proven honest.
 *
 * ```
 * nya_http_attestation_bundle_digest   one digest over a set of (path, bytes): the content a manifest names
 * nya_http_attestation_encode          the canonical, length-prefixed message a signature is taken over
 * nya_http_attestation_sign            an Ed25519 signature over that message, with the origin's key
 * nya_http_attestation_verify          whether a signature is the origin key's over exactly that manifest
 * nya_http_attestation_to_json         the manifest, the public key and the signature as a served document
 * nya_http_attestation_from_json       that document, fetched from a mirror, back into the three
 * ```
 *
 * ── what it is for ──
 *
 * A popular service grows mirrors: other hosts serving the same site, which is a good thing under Tor,
 * where reaching the one origin is slow and censorable. But a mirror is a machine somebody else runs,
 * and a client has no way to tell a faithful mirror from one that changed a link, an address, or a
 * download. An attestation closes that: the origin signs a statement of who it is and what it serves,
 * with a key only the origin holds, and publishes it at a well-known path. A verifier that has pinned
 * the origin's public key can fetch the statement from any mirror and check two things — that the origin
 * signed it, and that the mirror is actually serving the bytes the statement names — and so tell a
 * faithful mirror from a tampered one without trusting the mirror at all.
 *
 * It proves the bytes are the origin's; it does not prove they are safe or current beyond the manifest's
 * timestamp. A stale attestation served by a mirror is caught by that timestamp, not by this signature.
 *
 * ── the manifest, and why it is length-prefixed ──
 *
 * The signed message is a fixed, unambiguous serialization: a magic tag that is also the version and the
 * domain separator, then the origin as a length and its bytes, then the timestamp, then the content
 * digest as a length and its bytes. Every variable field carries its length ahead of it, so no two
 * different manifests serialize to the same bytes — the mistake a bare concatenation makes, where
 * "ab"+"c" and "a"+"bc" sign the same thing. A signature is only as honest as the message under it is
 * unambiguous; see nya_http_attestation_encode.
 *
 * The content digest is what nya_http_attestation_bundle_digest folds out of the served files: a hash
 * over each file's path and bytes, in the order the caller lists them. The origin computes it over the
 * bundle it mounts; a verifier recomputes it over the bytes it fetched from the mirror, and a mismatch
 * is a mirror serving something else.
 *
 * ── the key ──
 *
 * Ed25519 (crypto_sign.h). The secret key is the origin's own, and it is a configured secret, never a
 * checked-in one: an example that shipped one would be shipping the power to forge its own attestation.
 * The public half is pinned by whoever verifies — published out of band, in a client, in a directory —
 * and the whole point is that the verifier trusts that pinned key and nothing the mirror says. An
 * attestation carries the public key too, but a verifier compares it to the pinned one rather than
 * trusting it: a mirror that presents its own key and a matching signature has signed nothing the
 * verifier asked about.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_sign.h"

// CONSTANTS

/** The path an origin publishes its attestation at, and a mirror serves unchanged. */
#define NYA_HTTP_ATTESTATION_PATH "/.well-known/mirror-attestation"

/** The eight-byte tag at the head of every signed message: the format, its version, and its domain separator in one. */
#define NYA_HTTP_ATTESTATION_MAGIC      "NYAATST1"
#define NYA_HTTP_ATTESTATION_MAGIC_BYTES 8

/** The format version, carried in the served document so a reader can tell this shape from a later one. */
#define NYA_HTTP_ATTESTATION_VERSION 1

/** Bytes of the canonical origin string, terminator included: "http://<56 base32>.onion" and a little slack. */
#define NYA_HTTP_ATTESTATION_MAX_ORIGIN 256

/**
 * The largest a signed message can be: the magic, then a length and the origin, the timestamp, and a
 * length and the content digest. A fixed ceiling, so the message never allocates.
 * */
#define NYA_HTTP_ATTESTATION_MAX_MESSAGE (NYA_HTTP_ATTESTATION_MAGIC_BYTES + sizeof(u64) + NYA_HTTP_ATTESTATION_MAX_ORIGIN + sizeof(u64) + sizeof(u64) + NYA_CRYPTO_SHA256_BYTES)

// TYPES

typedef struct NYA_HttpAttestationManifest NYA_HttpAttestationManifest;
typedef struct NYA_HttpAttestationFile     NYA_HttpAttestationFile;

/** What the origin signs: who it is, when it said so, and a digest of what it serves. */
struct NYA_HttpAttestationManifest {
    /** The canonical origin the bytes are served from, e.g. "http://<hash>.onion". Bounded, NUL terminated. */
    char origin[NYA_HTTP_ATTESTATION_MAX_ORIGIN];

    /** Seconds since the epoch the manifest was made, so a stale attestation a mirror kept is caught. */
    u64 issued_at_s;

    /** A digest of the bytes served, from nya_http_attestation_bundle_digest or any SHA-256 the origin chooses. */
    NYA_CryptoSha256Digest content;
};

/** One file in a bundle, for the content digest: its path and its bytes. */
struct NYA_HttpAttestationFile {
    /** The path it is served at, folded into the digest so a file moving is a different bundle. */
    NYA_ConstCString path;

    const u8* data;
    u64       size;
};

// FUNCTIONS

/**
 * One SHA-256 over `files`, each contributing its path and its bytes, both length-prefixed, in the order
 * given. The digest a manifest's `content` is set to.
 *
 * The order is the caller's and is part of the digest: an origin and a verifier that list the same files
 * in the same order get the same digest, and a bundle whose files were reordered is a different one. List
 * them the way they are mounted, which is stable.
 * */
NYA_API void nya_http_attestation_bundle_digest(const NYA_HttpAttestationFile* files, u64 count, OUT NYA_CryptoSha256Digest* out_digest);

/**
 * Writes the canonical, length-prefixed message a signature is taken over into `out_message`, and its
 * length into `out_size`. See the header note for the layout and why it is unambiguous.
 *
 * Fails only for an `out_message` smaller than NYA_HTTP_ATTESTATION_MAX_MESSAGE or an origin that is not
 * NUL terminated inside its bound — a manifest this tree builds hits neither.
 * */
NYA_API NYA_Error nya_http_attestation_encode(const NYA_HttpAttestationManifest* manifest, OUT u8* out_message, u64 capacity, OUT u64* out_size)
    __attr_no_discard;

/** An Ed25519 signature over the canonical message of `manifest`, with the origin's secret key. */
NYA_API NYA_Error nya_http_attestation_sign(const NYA_CryptoSignSecretKey* secret_key, const NYA_HttpAttestationManifest* manifest, OUT NYA_CryptoSignature* out_signature)
    __attr_no_discard;

/**
 * Whether `signature` is `public_key`'s over exactly `manifest`. Constant in shape: it always re-encodes
 * the manifest and always runs one verify, so a tampered manifest and a wrong key both fail the same way,
 * with no early return that a caller could time. False for any altered byte, any wrong key, and a manifest
 * that will not encode.
 * */
NYA_API b8 nya_http_attestation_verify(const NYA_CryptoSignPublicKey* public_key, const NYA_HttpAttestationManifest* manifest, const NYA_CryptoSignature* signature)
    __attr_no_discard;

/**
 * The document an origin serves and a mirror passes through: the manifest, the public key, and the
 * signature, as a NYA_Object rendered later to JSON. The digest, key and signature are base64url.
 * */
NYA_API NYA_Error nya_http_attestation_to_json(
    NYA_Arena* arena, const NYA_HttpAttestationManifest* manifest, const NYA_CryptoSignPublicKey* public_key, const NYA_CryptoSignature* signature, OUT NYA_Object** out_object
) __attr_no_discard;

/**
 * The inverse: a fetched attestation document back into the manifest, the public key and the signature.
 *
 * Constant in shape — it decodes each field into a fixed buffer and checks the exact length — so a
 * document missing a field, carrying one of the wrong length, or with a digest, key or signature that is
 * not base64url of the right size is refused whole rather than half-read. Verifying what comes out is the
 * caller's next step, against the pinned key; this only parses.
 * */
NYA_API NYA_Error nya_http_attestation_from_json(
    const NYA_Object* object, OUT NYA_HttpAttestationManifest* out_manifest, OUT NYA_CryptoSignPublicKey* out_public_key, OUT NYA_CryptoSignature* out_signature
) __attr_no_discard;
