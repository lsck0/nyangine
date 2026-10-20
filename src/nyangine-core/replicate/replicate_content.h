/**
 * @file replicate_content.h
 *
 * What a player has to share with a server to play on it: the build, the baked asset bundle and every mod.
 * HELLO carries this process's manifest and its hash, and the server refuses a player whose content differs
 * from its own with NYA_NET_DISCONNECT_CONTENT, naming the first entry the two disagree on.
 *
 * Everything in the module:
 *   nya_net_content_collect            this process's manifest: build, asset bundle, every loaded plugin
 *   nya_net_content_add / _remove      one entry, kept in order; what collect, a game and a test build from
 *   nya_net_content_hash               SHA-256 over every entry
 *   nya_net_content_compare            the first entry a server's and a client's manifest disagree on
 *   nya_net_content_encode / _decode   the wire form that follows HELLO's document
 *   nya_net_content_mismatch_describe  one line for a log or a refusal screen
 *
 * ```c
 * NYA_NetContentManifest ours = { 0 };
 * NYA_EXPECT(nya_net_content_collect(&ours));
 *
 * NYA_NetContentMismatch why = nya_net_content_compare(&ours, &theirs);
 *
 * if (why.difference != NYA_NET_CONTENT_SAME) {
 *     char line[NYA_NET_CONTENT_DESCRIBE_MAX];
 *     (void)nya_net_content_mismatch_describe(&why, line, sizeof(line));
 *     nya_log_warn("Refusing a player: %s", line);
 * }
 * ```
 *
 * The entries ride beside the hash because a hash alone can only say "different", and a refusal that does not
 * say which mod to update sends a player hunting. Ten entries encode to at most NYA_NET_CONTENT_ENCODED_MAX
 * bytes, which UDP fragments like any reliable message.
 *
 * Every mod is compared, a HUD skin as much as a physics change. Whether a mod only changes what one player sees
 * could only be the mod's own claim, and a cheat would make it.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_plugin.h"
#include "nyangine-core/crypto/crypto_hash.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** An entry's id, terminator included: a plugin's name, or the fixed ids below. */
#define NYA_NET_CONTENT_ID_MAX NYA_PLUGIN_NAME_MAX

/** An entry's version, terminator included. Room for a build's `<version>+<commit>-dirty`. */
#define NYA_NET_CONTENT_VERSION_MAX 48

/** Every plugin that can be loaded, plus the build and the asset bundle. */
#define NYA_NET_CONTENT_ENTRY_MAX (NYA_PLUGIN_MAX + 2)

/** The largest encoding: the hash, the count, then per entry its kind, two lengths, both strings and a digest. */
#define NYA_NET_CONTENT_ENCODED_MAX \
    (NYA_CRYPTO_SHA256_BYTES + 1 + (NYA_NET_CONTENT_ENTRY_MAX * (3 + (NYA_NET_CONTENT_ID_MAX - 1) + (NYA_NET_CONTENT_VERSION_MAX - 1) + NYA_CRYPTO_SHA256_BYTES)))

/** What nya_net_content_mismatch_describe writes at most, terminator included. */
#define NYA_NET_CONTENT_DESCRIBE_MAX 192

/** The ids of the two entries that are not plugins. */
#define NYA_NET_CONTENT_ID_BUILD  "build"
#define NYA_NET_CONTENT_ID_ASSETS "assets"

/** The asset entry's version: whether this executable carries a baked bundle or reads assets off disk. */
#define NYA_NET_CONTENT_ASSETS_BAKED "baked"
#define NYA_NET_CONTENT_ASSETS_DISK  "disk"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_NetContentKind       NYA_NetContentKind;
typedef enum NYA_NetContentDifference NYA_NetContentDifference;
typedef struct NYA_NetContentEntry    NYA_NetContentEntry;
typedef struct NYA_NetContentManifest NYA_NetContentManifest;
typedef struct NYA_NetContentMismatch NYA_NetContentMismatch;

/** In the order a manifest sorts by, so the build is always the first difference named. */
enum NYA_NetContentKind {
    NYA_NET_CONTENT_BUILD = 0,
    NYA_NET_CONTENT_ASSETS,
    NYA_NET_CONTENT_PLUGIN,

    NYA_NET_CONTENT_KIND_COUNT,
};

/** Which way the first differing entry differs, seen from the server. */
enum NYA_NetContentDifference {
    NYA_NET_CONTENT_SAME = 0,

    /** The server has it and the client does not. */
    NYA_NET_CONTENT_MISSING,

    /** The client has it and the server does not. */
    NYA_NET_CONTENT_EXTRA,

    /** Both have it, at different versions. */
    NYA_NET_CONTENT_VERSION,

    /** Both have it at one version, with different bytes. */
    NYA_NET_CONTENT_DIGEST,

    NYA_NET_CONTENT_DIFFERENCE_COUNT,
};

struct NYA_NetContentEntry {
    NYA_NetContentKind kind;

    /** Printable ASCII, unique within its kind. */
    char id[NYA_NET_CONTENT_ID_MAX];
    char version[NYA_NET_CONTENT_VERSION_MAX];

    /** Zero for the build, whose version is its identity. */
    NYA_CryptoSha256Digest digest;
};

/** Sorted by kind, then id, by nya_net_content_add; decode refuses any other order, so two equal sets encode alike. */
struct NYA_NetContentManifest {
    NYA_NetContentEntry entries[NYA_NET_CONTENT_ENTRY_MAX];
    u32                 entry_count;
};

/** The first entry two manifests disagree on, or `difference` NYA_NET_CONTENT_SAME. */
struct NYA_NetContentMismatch {
    NYA_NetContentDifference difference;
    NYA_NetContentKind       kind;

    char id[NYA_NET_CONTENT_ID_MAX];

    /** Empty on the side that lacks the entry. */
    char server_version[NYA_NET_CONTENT_VERSION_MAX];
    char client_version[NYA_NET_CONTENT_VERSION_MAX];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Inserts `entry` in order. NYA_ERROR_INVALID_ARGUMENT for an empty or unprintable id or version, or an unknown kind;
 * NYA_ERROR_ALREADY_EXISTS for an id its kind already has; NYA_ERROR_OUT_OF_MEMORY past NYA_NET_CONTENT_ENTRY_MAX.
 * */
NYA_API NYA_Error nya_net_content_add(NYA_NetContentManifest* manifest, NYA_NetContentEntry entry) __attr_no_discard;

/** Takes the entry of `kind` called `id` out, keeping the rest in order. False when there was none. */
NYA_API b8 nya_net_content_remove(NYA_NetContentManifest* manifest, NYA_NetContentKind kind, NYA_ConstCString id);

/**
 * This process's content, replacing what `out_manifest` held: the build as `<version>+<commit>`, the asset bundle
 * (nya_asset_bundle_digest), and every loaded plugin with the content digest it was loaded with.
 * */
NYA_API NYA_Error nya_net_content_collect(OUT NYA_NetContentManifest* out_manifest) __attr_no_discard;

/** SHA-256 over every entry, in order, each field length-prefixed. */
NYA_API void nya_net_content_hash(const NYA_NetContentManifest* manifest, OUT NYA_CryptoSha256Digest* out_hash);

/** The first entry, in manifest order, on which `server` and `client` disagree. */
NYA_API NYA_NetContentMismatch nya_net_content_compare(const NYA_NetContentManifest* server, const NYA_NetContentManifest* client) __attr_no_discard;

/** Appends the hash, then the entries. At most NYA_NET_CONTENT_ENCODED_MAX bytes. */
NYA_API void nya_net_content_encode(NYA_String* out, const NYA_NetContentManifest* manifest);

/**
 * Reads exactly `size` bytes back. Refuses an unknown kind, an empty or unprintable string, entries out of
 * order or repeated, trailing bytes, and a hash that is not the hash of the entries it came with.
 * */
NYA_API NYA_Error nya_net_content_decode(const u8* data, u64 size, OUT NYA_NetContentManifest* out_manifest) __attr_no_discard;

/** "plugin 'hello' is 0.2.0 on the server and 0.1.0 here", and the like. Returns the bytes written. */
NYA_API u64 nya_net_content_mismatch_describe(const NYA_NetContentMismatch* mismatch, OUT char* out, u64 capacity);
