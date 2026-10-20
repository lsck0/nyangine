#include "nyangine-core/nyangine.h"

#include "nyangine-core/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ENCODING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ```
 * hash      32 bytes   nya_net_content_hash of the entries that follow
 * count     u8         at most NYA_NET_CONTENT_ENTRY_MAX
 * per entry
 *   kind    u8
 *   flags   u8         bit 0: client-only
 *   id      u8 length, then that many bytes, at least one
 *   version u8 length, then that many bytes, at least one
 *   digest  32 bytes
 * ```
 */

#define _NYA_NET_CONTENT_FLAG_CLIENT_ONLY (1U << 0)

/** Fed into the hash first, so a manifest hash never equals a SHA-256 taken of the same bytes for another reason. */
#define _NYA_NET_CONTENT_HASH_DOMAIN "nyangine.net.content.v1"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Non-empty, terminated inside `capacity`, and printable ASCII throughout: what logs and refusal screens show. */
NYA_INTERNAL b8 _nya_net_content_string_is_valid(const char* text, u64 capacity) __attr_no_discard;

/** Copies the printable ASCII of `text` into `out`, dropping everything else. What a peer's string becomes before it is shown. */
NYA_INTERNAL void _nya_net_content_copy_printable(OUT char* out, u64 capacity, NYA_ConstCString text);

/** Manifest order: kind, then id in byte order. */
NYA_INTERNAL s32 _nya_net_content_order(const NYA_NetContentEntry* a, const NYA_NetContentEntry* b) __attr_no_discard;

/** The next entry at or after `*at` that counts under `policy`, advancing `*at` past it, or null at the end. */
NYA_INTERNAL const NYA_NetContentEntry* _nya_net_content_next(const NYA_NetContentManifest* manifest, u32* at, NYA_NetContentPolicy policy);

NYA_INTERNAL NYA_ConstCString _nya_net_content_kind_name(NYA_NetContentKind kind) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_content_add(NYA_NetContentManifest* manifest, NYA_NetContentEntry entry) {
    nya_assert(manifest != nullptr);
    nya_assert(manifest->entry_count <= NYA_NET_CONTENT_ENTRY_MAX);

    if ((u32)entry.kind >= NYA_NET_CONTENT_KIND_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "content of kind %u", (u32)entry.kind);
    if (!_nya_net_content_string_is_valid(entry.id, sizeof(entry.id))) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a content id that is empty or unprintable");
    if (!_nya_net_content_string_is_valid(entry.version, sizeof(entry.version))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has a version that is empty or unprintable", entry.id);
    }

    if (manifest->entry_count == NYA_NET_CONTENT_ENTRY_MAX) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit: a manifest holds %d entries", entry.id, NYA_NET_CONTENT_ENTRY_MAX);
    }

    u32 at = 0;
    while (at < manifest->entry_count && _nya_net_content_order(&manifest->entries[at], &entry) < 0) at++;

    if (at < manifest->entry_count && _nya_net_content_order(&manifest->entries[at], &entry) == 0) {
        return nya_error(NYA_ERROR_ALREADY_EXISTS, "%s '%s' is already in the manifest", _nya_net_content_kind_name(entry.kind), entry.id);
    }

    for (u32 i = manifest->entry_count; i > at; i--) manifest->entries[i] = manifest->entries[i - 1];

    manifest->entries[at] = entry;
    manifest->entry_count++;

    nya_assert(at == 0 || _nya_net_content_order(&manifest->entries[at - 1], &manifest->entries[at]) < 0);

    return NYA_OK;
}

b8 nya_net_content_remove(NYA_NetContentManifest* manifest, NYA_NetContentKind kind, NYA_ConstCString id) {
    nya_assert(manifest != nullptr && id != nullptr);
    nya_assert(manifest->entry_count <= NYA_NET_CONTENT_ENTRY_MAX);

    for (u32 at = 0; at < manifest->entry_count; at++) {
        const NYA_NetContentEntry* entry = &manifest->entries[at];
        if (entry->kind != kind || strncmp(entry->id, id, sizeof(entry->id)) != 0) continue;

        for (u32 i = at; i + 1 < manifest->entry_count; i++) manifest->entries[i] = manifest->entries[i + 1];

        manifest->entry_count--;
        manifest->entries[manifest->entry_count] = (NYA_NetContentEntry){ 0 };

        return true;
    }

    return false;
}

NYA_Error nya_net_content_collect(OUT NYA_NetContentManifest* out_manifest) {
    nya_assert(out_manifest != nullptr);

    *out_manifest = (NYA_NetContentManifest){ 0 };

    NYA_BuildInfo        info  = nya_build_info();
    NYA_NetContentEntry  build = { .kind = NYA_NET_CONTENT_BUILD, .id = NYA_NET_CONTENT_ID_BUILD };
    (void)snprintf(build.version, sizeof(build.version), "%s+%s", info.version, info.commit);
    NYA_TRY(nya_net_content_add(out_manifest, build));

    NYA_NetContentEntry assets = { .kind = NYA_NET_CONTENT_ASSETS, .id = NYA_NET_CONTENT_ID_ASSETS };
    b8 baked = nya_asset_bundle_digest(&assets.digest);
    (void)snprintf(assets.version, sizeof(assets.version), "%s", baked ? NYA_NET_CONTENT_ASSETS_BAKED : NYA_NET_CONTENT_ASSETS_DISK);
    NYA_TRY(nya_net_content_add(out_manifest, assets));

    for (u32 i = 0; i < nya_plugin_count(); i++) {
        const NYA_Plugin* plugin = nya_plugin_at(i);

        NYA_NetContentEntry entry = { .kind = NYA_NET_CONTENT_PLUGIN, .digest = plugin->content_digest, .client_only = plugin->manifest.client_only };
        _nya_net_content_copy_printable(entry.id, sizeof(entry.id), plugin->manifest.name);
        _nya_net_content_copy_printable(entry.version, sizeof(entry.version), plugin->manifest.version);

        NYA_TRY(nya_net_content_add(out_manifest, entry));
    }

    return NYA_OK;
}

void nya_net_content_hash(const NYA_NetContentManifest* manifest, OUT NYA_CryptoSha256Digest* out_hash) {
    nya_assert(manifest != nullptr && out_hash != nullptr);
    nya_assert(manifest->entry_count <= NYA_NET_CONTENT_ENTRY_MAX);

    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);
    nya_crypto_sha256_update(&sha256, (const u8*)_NYA_NET_CONTENT_HASH_DOMAIN, sizeof(_NYA_NET_CONTENT_HASH_DOMAIN) - 1);

    for (u32 i = 0; i < manifest->entry_count; i++) {
        const NYA_NetContentEntry* entry = &manifest->entries[i];
        if (entry->client_only) continue;

        u8 id_length      = (u8)strnlen(entry->id, sizeof(entry->id));
        u8 version_length = (u8)strnlen(entry->version, sizeof(entry->version));
        u8 kind           = (u8)entry->kind;

        // lengths in front, so no two different entries feed the same bytes.
        nya_crypto_sha256_update(&sha256, &kind, 1);
        nya_crypto_sha256_update(&sha256, &id_length, 1);
        nya_crypto_sha256_update(&sha256, (const u8*)entry->id, id_length);
        nya_crypto_sha256_update(&sha256, &version_length, 1);
        nya_crypto_sha256_update(&sha256, (const u8*)entry->version, version_length);
        nya_crypto_sha256_update(&sha256, entry->digest.bytes, sizeof(entry->digest.bytes));
    }

    nya_crypto_sha256_end(&sha256, out_hash);
}

NYA_NetContentMismatch nya_net_content_compare(const NYA_NetContentManifest* server, const NYA_NetContentManifest* client, NYA_NetContentPolicy policy) {
    nya_assert(server != nullptr && client != nullptr);

    u32 server_at = 0;
    u32 client_at = 0;

    const NYA_NetContentEntry* ours   = _nya_net_content_next(server, &server_at, policy);
    const NYA_NetContentEntry* theirs = _nya_net_content_next(client, &client_at, policy);

    // a merge of two sorted lists: at most every entry of both, each step consuming at least one.
    while (ours != nullptr || theirs != nullptr) {
        s32 order = ours == nullptr ? 1 : theirs == nullptr ? -1 : _nya_net_content_order(ours, theirs);

        NYA_NetContentMismatch mismatch = { .difference = NYA_NET_CONTENT_SAME };

        if (order < 0) {
            mismatch = (NYA_NetContentMismatch){ .difference = NYA_NET_CONTENT_MISSING, .kind = ours->kind };
        } else if (order > 0) {
            mismatch = (NYA_NetContentMismatch){ .difference = NYA_NET_CONTENT_EXTRA, .kind = theirs->kind };
        } else if (!nya_string_equals(ours->version, theirs->version)) {
            mismatch = (NYA_NetContentMismatch){ .difference = NYA_NET_CONTENT_VERSION, .kind = ours->kind };
        } else if (nya_memcmp(ours->digest.bytes, theirs->digest.bytes, sizeof(ours->digest.bytes)) != 0) {
            mismatch = (NYA_NetContentMismatch){ .difference = NYA_NET_CONTENT_DIGEST, .kind = ours->kind };
        }

        if (mismatch.difference != NYA_NET_CONTENT_SAME) {
            const NYA_NetContentEntry* named = order > 0 ? theirs : ours;
            (void)snprintf(mismatch.id, sizeof(mismatch.id), "%s", named->id);

            if (order <= 0) (void)snprintf(mismatch.server_version, sizeof(mismatch.server_version), "%s", ours->version);
            if (order >= 0) (void)snprintf(mismatch.client_version, sizeof(mismatch.client_version), "%s", theirs->version);

            return mismatch;
        }

        ours   = _nya_net_content_next(server, &server_at, policy);
        theirs = _nya_net_content_next(client, &client_at, policy);
    }

    return (NYA_NetContentMismatch){ .difference = NYA_NET_CONTENT_SAME };
}

void nya_net_content_encode(NYA_String* out, const NYA_NetContentManifest* manifest) {
    nya_assert(out != nullptr && manifest != nullptr);
    nya_assert(manifest->entry_count <= NYA_NET_CONTENT_ENTRY_MAX);

    u64 start = out->length;

    NYA_CryptoSha256Digest hash = { 0 };
    nya_net_content_hash(manifest, &hash);

    for (u32 i = 0; i < sizeof(hash.bytes); i++) nya_string_push_back(out, hash.bytes[i]);

    nya_string_push_back(out, (u8)manifest->entry_count);

    for (u32 i = 0; i < manifest->entry_count; i++) {
        const NYA_NetContentEntry* entry = &manifest->entries[i];

        u8 id_length      = (u8)strnlen(entry->id, sizeof(entry->id));
        u8 version_length = (u8)strnlen(entry->version, sizeof(entry->version));

        nya_string_push_back(out, (u8)entry->kind);
        nya_string_push_back(out, (u8)(entry->client_only ? _NYA_NET_CONTENT_FLAG_CLIENT_ONLY : 0U));
        nya_string_push_back(out, id_length);
        for (u32 c = 0; c < id_length; c++) nya_string_push_back(out, (u8)entry->id[c]);
        nya_string_push_back(out, version_length);
        for (u32 c = 0; c < version_length; c++) nya_string_push_back(out, (u8)entry->version[c]);
        for (u32 b = 0; b < sizeof(entry->digest.bytes); b++) nya_string_push_back(out, entry->digest.bytes[b]);
    }

    nya_assert(out->length - start <= NYA_NET_CONTENT_ENCODED_MAX);
}

NYA_Error nya_net_content_decode(const u8* data, u64 size, OUT NYA_NetContentManifest* out_manifest) {
    nya_assert(out_manifest != nullptr);

    *out_manifest = (NYA_NetContentManifest){ 0 };

    if (data == nullptr || size > NYA_NET_CONTENT_ENCODED_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a content manifest of %llu bytes", (unsigned long long)size);

    _NYA_NetReader reader = { .data = data, .size = size };

    NYA_CryptoSha256Digest claimed = { 0 };
    if (!_nya_net_reader_has(&reader, sizeof(claimed.bytes))) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a content manifest with no hash");
    nya_memcpy(claimed.bytes, &data[reader.at], sizeof(claimed.bytes));
    reader.at += sizeof(claimed.bytes);

    u32 count = _nya_net_read_u8(&reader);
    if (reader.failed || count > NYA_NET_CONTENT_ENTRY_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a content manifest of %u entries", count);

    for (u32 i = 0; i < count; i++) {
        NYA_NetContentEntry entry = { 0 };

        u8 kind  = _nya_net_read_u8(&reader);
        u8 flags = _nya_net_read_u8(&reader);

        if (reader.failed || kind >= NYA_NET_CONTENT_KIND_COUNT || (flags & ~_NYA_NET_CONTENT_FLAG_CLIENT_ONLY) != 0) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "content entry %u has kind %u and flags %u", i, kind, flags);
        }

        entry.kind        = (NYA_NetContentKind)kind;
        entry.client_only = (flags & _NYA_NET_CONTENT_FLAG_CLIENT_ONLY) != 0;

        // both strings the same way: a length that leaves room for the terminator, then exactly that many bytes.
        char* fields[2]    = { entry.id, entry.version };
        u64   capacities[2] = { sizeof(entry.id), sizeof(entry.version) };

        for (u32 f = 0; f < 2; f++) {
            u8 length = _nya_net_read_u8(&reader);

            if (reader.failed || length == 0 || length >= capacities[f] || !_nya_net_reader_has(&reader, length)) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "content entry %u has a string of %u bytes", i, length);
            }

            nya_memcpy(fields[f], &data[reader.at], length);
            fields[f][length]  = '\0';
            reader.at         += length;
        }

        if (!_nya_net_reader_has(&reader, sizeof(entry.digest.bytes))) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "content entry %u is truncated", i);
        nya_memcpy(entry.digest.bytes, &data[reader.at], sizeof(entry.digest.bytes));
        reader.at += sizeof(entry.digest.bytes);

        // in order, so one set has one encoding and a repeat cannot hide behind a later entry.
        if (out_manifest->entry_count > 0 && _nya_net_content_order(&out_manifest->entries[out_manifest->entry_count - 1], &entry) >= 0) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "content entry %u is out of order", i);
        }

        NYA_Error added = nya_net_content_add(out_manifest, entry);
        if (!added.ok) return added;
    }

    if (reader.at != size) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a content manifest with %llu bytes past its end", (unsigned long long)(size - reader.at));

    NYA_CryptoSha256Digest actual = { 0 };
    nya_net_content_hash(out_manifest, &actual);

    if (nya_memcmp(actual.bytes, claimed.bytes, sizeof(actual.bytes)) != 0) {
        *out_manifest = (NYA_NetContentManifest){ 0 };
        return nya_error(NYA_ERROR_CORRUPT, "a content manifest whose hash is not its entries'");
    }

    nya_assert(out_manifest->entry_count == count);

    return NYA_OK;
}

u64 nya_net_content_mismatch_describe(const NYA_NetContentMismatch* mismatch, OUT char* out, u64 capacity) {
    nya_assert(mismatch != nullptr && out != nullptr && capacity > 0);

    // the strings may have come off the wire, so only what they hold of printable ASCII is shown.
    char id[NYA_NET_CONTENT_ID_MAX];
    char server_version[NYA_NET_CONTENT_VERSION_MAX];
    char client_version[NYA_NET_CONTENT_VERSION_MAX];
    _nya_net_content_copy_printable(id, sizeof(id), mismatch->id);
    _nya_net_content_copy_printable(server_version, sizeof(server_version), mismatch->server_version);
    _nya_net_content_copy_printable(client_version, sizeof(client_version), mismatch->client_version);

    NYA_ConstCString kind = (u32)mismatch->kind < NYA_NET_CONTENT_KIND_COUNT ? _nya_net_content_kind_name(mismatch->kind) : "content";

    // a plugin is named; the build and the bundle are one of a kind.
    char subject[NYA_NET_CONTENT_ID_MAX + 32];
    if (mismatch->kind == NYA_NET_CONTENT_PLUGIN) (void)snprintf(subject, sizeof(subject), "%s '%s'", kind, id);
    else (void)snprintf(subject, sizeof(subject), "the %s", kind);

    s32 written = 0;

    switch (mismatch->difference) {
        case NYA_NET_CONTENT_SAME:    written = snprintf(out, capacity, "the content matches"); break;
        case NYA_NET_CONTENT_MISSING: written = snprintf(out, capacity, "the server has %s %s, which you do not", subject, server_version); break;
        case NYA_NET_CONTENT_EXTRA:   written = snprintf(out, capacity, "you have %s %s, which the server does not", subject, client_version); break;
        case NYA_NET_CONTENT_VERSION: written = snprintf(out, capacity, "%s is %s on the server and %s here", subject, server_version, client_version); break;
        case NYA_NET_CONTENT_DIGEST:  written = snprintf(out, capacity, "%s %s differs from the server's copy", subject, server_version); break;

        case NYA_NET_CONTENT_DIFFERENCE_COUNT:
        default:                               written = snprintf(out, capacity, "the content differs"); break;
    }

    if (written < 0) {
        out[0] = '\0';
        return 0;
    }

    return nya_min((u64)written, capacity - 1);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_net_content_string_is_valid(const char* text, u64 capacity) {
    nya_assert(text != nullptr);

    u64 length = strnlen(text, capacity);
    if (length == 0 || length == capacity) return false;

    for (u64 i = 0; i < length; i++) {
        u8 character = (u8)text[i];
        if (character < 0x20 || character > 0x7E) return false;
    }

    return true;
}

void _nya_net_content_copy_printable(OUT char* out, u64 capacity, NYA_ConstCString text) {
    nya_assert(out != nullptr && capacity > 0);

    u64 written = 0;

    for (u64 i = 0; text != nullptr && text[i] != '\0' && written + 1 < capacity; i++) {
        u8 character = (u8)text[i];
        if (character >= 0x20 && character <= 0x7E) out[written++] = (char)character;
    }

    out[written] = '\0';
}

s32 _nya_net_content_order(const NYA_NetContentEntry* a, const NYA_NetContentEntry* b) {
    nya_assert(a != nullptr && b != nullptr);

    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;

    s32 difference = strncmp(a->id, b->id, sizeof(a->id));

    return difference < 0 ? -1 : difference > 0 ? 1 : 0;
}

const NYA_NetContentEntry* _nya_net_content_next(const NYA_NetContentManifest* manifest, u32* at, NYA_NetContentPolicy policy) {
    nya_assert(manifest != nullptr && at != nullptr);

    while (*at < manifest->entry_count) {
        const NYA_NetContentEntry* entry = &manifest->entries[*at];
        (*at)++;

        if (policy.allow_client_only && entry->client_only) continue;

        return entry;
    }

    return nullptr;
}

NYA_ConstCString _nya_net_content_kind_name(NYA_NetContentKind kind) {
    switch (kind) {
        case NYA_NET_CONTENT_BUILD:  return "build";
        case NYA_NET_CONTENT_ASSETS: return "asset bundle";
        case NYA_NET_CONTENT_PLUGIN: return "plugin";

        case NYA_NET_CONTENT_KIND_COUNT:
        default:                         nya_assert(false, "content of kind %d", (int)kind); return "content";
    }
}
