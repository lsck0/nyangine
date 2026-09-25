/**
 * @file core_plugin_signature.h
 *
 * Who wrote a plugin, proven before a line of it runs.
 *
 * core_plugin.h says it plainly: "Nothing is verified. No signature, no checksum." A plugin is a folder
 * somebody else put on disk, and a folder can be replaced by anything with write access to it. This is
 * the answer to that one sentence — an Ed25519 signature over the plugin, checked at load against a set
 * of publisher keys the *program* pins, so a plugin the program does not trust does not run.
 *
 * Everything in the module:
 *   nya_plugin_digest            the one canonical hash of a plugin's contents, signer and loader alike
 *   nya_plugin_trust_key         pin a publisher's public key; the host calls this, nothing else can
 *   nya_plugin_signature_verify  whether a plugin's `plugin.sig` is a pinned key's over its digest
 *   nya_plugin_signature_write   produce that `plugin.sig`, which is what the signing tool calls
 *
 * ─────────────────────────────────────────────────────────
 * WHAT IS SIGNED, AND HOW A KEY IS PINNED
 * ─────────────────────────────────────────────────────────
 *
 * **What.** Not the folder byte for byte — a folder has no defined order and carries files that do not
 * run. What is signed is a digest (nya_plugin_digest) over exactly the plugin's *code and its manifest*:
 * `manifest.nya`, then `main.lua`, then every `.lua` under `src/` in name order, each contributing its
 * relative path and its bytes with a length between them so no two layouts collide. That is the whole of
 * what nya_plugin_load will execute, so a signature over it is a signature over what the plugin does.
 * `plugin.sig` and everything under `assets/` are excluded: the first is the signature and the second is
 * data the plugin only reaches through the filesystem permission it had to be granted anyway.
 *
 * **How pinned.** A public key becomes trusted only by nya_plugin_trust_key, and only the host program's
 * own compiled code calls it — at startup, from a key literal in the binary, or from an index it fetched
 * and checked. No manifest, no config file and no plugin can reach it, which is the whole point: the set
 * of who-may-sign is the program's decision, fixed in the program, exactly like the permission profile.
 * Only public keys are ever pinned; a private key belongs on the developer's machine and in nothing that
 * ships. See secrets/README.md for where the signing key lives.
 *
 * ─────────────────────────────────────────────────────────
 * THE LOAD POLICY, AND THE ESCAPE HATCH
 * ─────────────────────────────────────────────────────────
 *
 * nya_plugin_load verifies before it runs anything. A plugin whose signature is a pinned key's over its
 * digest loads. A plugin that is unsigned, tampered with, or signed by a key the program does not pin is
 * REFUSED — that is the default, and it needs no configuration.
 *
 * The one way to run an unsigned plugin is to compile the program with `-DNYA_PLUGIN_REQUIRE_SIGNATURE=false`.
 * This is the developer's opt-in: it is off by default, and when it is on the loader still refuses
 * nothing silently — it logs a loud line per unsigned plugin naming it and every permission it is about
 * to hold without a signature, so the log records exactly what was trusted and on whose say-so. It is for
 * iterating on a plugin you are writing, and it is not what ships.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_sign.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The signature, beside `manifest.nya` in the plugin's own directory. Text, so it can be read by eye. */
#define NYA_PLUGIN_SIGNATURE_FILE "plugin.sig"

/** A publisher name in a signature: who a pinned key belongs to, and what `plugin.sig` records. */
#define NYA_PLUGIN_PUBLISHER_MAX 96

/**
 * How many publisher keys the program may pin. A program trusts a handful of publishers, not a directory
 * of them; past this nya_plugin_trust_key refuses rather than growing, so the trusted set has a ceiling
 * a reader can hold in their head.
 * */
#ifndef NYA_PLUGIN_TRUSTED_KEY_MAX
#define NYA_PLUGIN_TRUSTED_KEY_MAX 16
#endif

/**
 * Whether an unsigned or untrusted plugin is refused. True by default and on purpose: the secure posture
 * is the one you get without configuring anything. Compile `-DNYA_PLUGIN_REQUIRE_SIGNATURE=false` to load
 * unsigned plugins, which the loader then does loudly; see the header comment.
 * */
#ifndef NYA_PLUGIN_REQUIRE_SIGNATURE
#define NYA_PLUGIN_REQUIRE_SIGNATURE true
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DIGEST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The one canonical SHA-256 over the plugin in `directory`: a domain tag, then `manifest.nya`, `main.lua`
 * and every `.lua` under `src/` in name order, each fed as its relative path, a length, and its bytes. The signer
 * and the loader both call this, so the value they sign and check cannot drift.
 *
 * NYA_ERROR_NOT_FOUND when there is no `manifest.nya` or no `main.lua`: a directory missing either is not
 * a plugin, and hashing it would produce a digest for nothing.
 * */
NYA_API NYA_Error nya_plugin_digest(NYA_ConstCString directory, OUT NYA_CryptoSha256Digest* out_digest) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PINNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Pins `public_key` as `publisher`'s, so a plugin signed by it will verify. Meant to be called by the
 * host program at startup, from a key compiled into it: there is deliberately no binding, config key or
 * manifest field that reaches this, because the set of publishers a program trusts is the program's to
 * decide and nothing a plugin controls may widen it.
 *
 * NYA_ERROR_OUT_OF_MEMORY once NYA_PLUGIN_TRUSTED_KEY_MAX are pinned. Pinning the same publisher twice
 * replaces its key rather than adding a second entry, so a rotation is one more call, not a leak.
 * */
NYA_API NYA_Error nya_plugin_trust_key(NYA_ConstCString publisher, const NYA_CryptoSignPublicKey* public_key) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VERIFYING AND SIGNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether `directory` carries a `plugin.sig` that a pinned key signed over the plugin's digest, and on
 * success the name of the publisher whose key it was in `out_publisher` (optional; pass null to ignore).
 *
 *   NYA_OK                     verified against a pinned key
 *   NYA_ERROR_NOT_FOUND        no `plugin.sig` — the plugin is unsigned
 *   NYA_ERROR_PARSE            a `plugin.sig` that is not the format nya_plugin_signature_write writes
 *   NYA_ERROR_PERMISSION_DENIED signed, but by no pinned key, or the signature does not match the digest
 *
 * A caller that must refuse everything but NYA_OK gets exactly that by testing `.ok`; the codes are for a
 * message that tells the difference between "you forgot to sign it" and "this is not who it claims to be".
 * */
NYA_API NYA_Error nya_plugin_signature_verify(NYA_ConstCString directory, OUT char* out_publisher, u64 capacity) __attr_no_discard;

/**
 * Signs the plugin in `directory` with `secret_key` and writes `plugin.sig` naming `publisher` and the
 * matching `public_key`. What the signing tool calls once it has read a developer's key off disk.
 *
 * The digest it signs is nya_plugin_digest's, so a `plugin.sig` this writes is one nya_plugin_signature_verify
 * accepts against `public_key` once that key is pinned. The secret key is the caller's to wipe; this reads
 * it and keeps nothing.
 * */
NYA_API NYA_Error nya_plugin_signature_write(
    NYA_ConstCString               directory,
    NYA_ConstCString               publisher,
    const NYA_CryptoSignSecretKey* secret_key,
    const NYA_CryptoSignPublicKey* public_key
) __attr_no_discard;

#ifdef NYA_TESTING
/** Drops every pinned key, so a test can start from an empty trust set. */
NYA_INTERNAL void _nya_plugin_trusted_keys_reset(void);
#endif
