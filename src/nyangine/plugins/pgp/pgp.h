/**
 * @file pgp.h
 *
 * OpenPGP, as far as this engine needs it: encrypting a short message to somebody's public key, so
 * that only the holder of the matching private key can read it.
 *
 * ```c
 * if (!nya_pgp_available()) return nya_error(NYA_ERROR_NOT_SUPPORTED, "this machine has no gpg");
 *
 * NYA_String* armored = nullptr;
 * NYA_TRY(nya_pgp_encrypt(arena, recipient_key, (const u8*)code, strlen(code), &armored));
 * ```
 *
 * ── why there is no OpenPGP implementation here ──
 *
 * The one thing the engine does with OpenPGP is the second factor in http_auth.h's design: the server
 * encrypts a one-time code to a user's published key, the user decrypts it with their own `gpg` or an
 * OpenPGP smartcard, and types the code back. Every private key operation in that flow happens on the
 * user's machine. The server does one public key operation and a constant time comparison, and it
 * never holds a secret of anybody's.
 *
 * So what this needs is an encryptor, and the options for getting one were:
 *
 * - **`gpg` as a process**, which is what this does. No vendored dependency, no link, no licence to
 *   reason about, and it speaks every key version anybody's key might be — including the v6 keys of
 *   RFC 9580, on the day a distribution ships a gpg that makes them. A machine without gpg answers
 *   "not available" and the program decides what that means, exactly as a missing GPU backend does.
 * - **GPGME**, the library. It is a wrapper that drives this same binary through an IPC protocol, and
 *   it would add libassuan and libgpg-error to the link for one call a minute. It buys a typed API
 *   over a process this can spawn in twenty lines.
 * - **RNP** (Thunderbird's), which is C++ with CMake and a crypto backend of its own — Botan or
 *   OpenSSL — duplicating a crypto module this engine already has and wrote itself.
 * - **Sequoia**, which is Rust, and would put a second toolchain in a build that is one clang.
 * - **Writing it**, which for encryption alone means OpenPGP packet writing, ECDH with a key
 *   derivation nobody else uses, RSA PKCS#1 v1.5 *encryption* (this engine only verifies), AES in a
 *   mode this has none of, and an interoperability surface that is where OpenPGP bugs live. It is the
 *   right answer only if the dependency below ever becomes unacceptable.
 *
 * ── the dependency, said plainly ──
 *
 * A build that wants this needs `gpg` on the machine at run time. It is installed on essentially every
 * Linux, is `gpg4win` on Windows, and is absent from a container nobody put it in. Nothing links
 * against it: this spawns it, hands it two files and reads a third, so a machine without it loses this
 * one feature and nothing else.
 *
 * ── what the server never does ──
 *
 * Decrypt. There is deliberately no counterpart to nya_pgp_encrypt here, because the counterpart runs
 * on the user's machine against a key this program must never see. A smartcard's PIN, a YubiKey's
 * touch policy and the agent that drives them are all on that side of the line.
 *
 * ── the temporary directory ──
 *
 * gpg is given a `--homedir` of its own for the length of one call, so this never reads, writes or
 * locks the keyring of whatever user the server runs as. The recipient's key and the plaintext go in
 * as files because gpg takes them that way; both are wiped and the directory is removed before the
 * call returns, whatever the outcome.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The largest message this encrypts, in bytes.
 *
 * A one-time code and the sentence around it saying what is being approved. This is not a file
 * encryptor: everything about the shape here — a temporary directory per call, a whole process
 * spawned — is sized for a few hundred bytes a minute, and a caller with a megabyte wants something
 * else entirely.
 * */
#define NYA_PGP_MAX_MESSAGE_BYTES 4096

/**
 * The largest public key this hands to gpg, in bytes.
 *
 * An armored transferable public key with a few subkeys and a photo id is under sixteen kilobytes;
 * past that it is either not a key or not one worth accepting from a user's profile form.
 * */
#define NYA_PGP_MAX_KEY_BYTES 16384

/**
 * How long gpg is given before it is a failure rather than a slow machine.
 *
 * Encryption to a known key is milliseconds of work. The rest of this is a process start and a
 * directory, so five seconds is far past a slow disk and short enough that a login does not hang.
 * */
#define NYA_PGP_TIMEOUT_MS 5000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether this machine has a gpg that works, asked once and remembered.
 *
 * What a program checks at startup to decide whether to offer a PGP second factor at all, rather than
 * offering one and failing at the moment somebody tries to log in.
 * */
NYA_API b8 nya_pgp_available(void) __attr_no_discard;

/** The version gpg reported, or an empty string when there is none. For a log line and an overlay. */
NYA_API NYA_ConstCString nya_pgp_version(void) __attr_no_discard;

/**
 * Encrypts `message` to `recipient_key`, an armored OpenPGP public key, and answers the armored
 * result.
 *
 * The key is used as given and is never imported into any keyring, so a user changing their key
 * changes nothing on this machine. A key gpg will not read, a key with nothing to encrypt to, and an
 * expired or revoked one are all refused with what gpg said about it.
 * */
NYA_API NYA_Error nya_pgp_encrypt(NYA_Arena* arena, NYA_ConstCString recipient_key, const u8* message, u64 message_size, OUT NYA_String** out_armored)
    __attr_no_discard;

/**
 * The fingerprint of the primary key in `recipient_key`, as uppercase hex.
 *
 * What a program stores against an account, and what it shows the user so they can check that the key
 * it will encrypt to is the key they meant. Refuses anything gpg cannot read as a key.
 * */
NYA_API NYA_Error nya_pgp_fingerprint(NYA_Arena* arena, NYA_ConstCString recipient_key, OUT NYA_String** out_fingerprint) __attr_no_discard;
