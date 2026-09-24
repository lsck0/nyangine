#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Fed into the hash before anything else, so this engine's plugin digest can never equal a bare SHA-256
 * of the same bytes computed for another purpose. The version rides in it: change what is signed, change
 * this string, and an old signature stops verifying rather than verifying something it no longer covers.
 * */
#define _NYA_PLUGIN_DIGEST_DOMAIN "nyangine.plugin.signature.v1"

/** The first line of `plugin.sig`, which says what the file is and which layout the rest of it uses. */
#define _NYA_PLUGIN_SIGNATURE_MAGIC "nyangine-plugin-signature 1"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One pinned publisher: a name to report and the public key a plugin's signature is checked against. */
typedef struct {
    char                    publisher[NYA_PLUGIN_PUBLISHER_MAX];
    NYA_CryptoSignPublicKey key;
    b8                      used;
} _NYA_PluginTrustedKey;

/* The trusted set. Zeroed is empty, which is the honest default: a program trusts nobody until its own
 * code pins somebody. */
NYA_INTERNAL _NYA_PluginTrustedKey _nya_plugin_trusted_keys[NYA_PLUGIN_TRUSTED_KEY_MAX] = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The value of one lower or upper case hex digit, or -1 for anything that is not one. */
NYA_INTERNAL s32 _nya_plugin_hex_value(char character) __attr_no_discard;

/** Exactly `size * 2` hex characters into `size` bytes. False for a wrong length or a non-hex character. */
NYA_INTERNAL b8 _nya_plugin_hex_decode(NYA_ConstCString hex, u64 hex_length, OUT u8* out, u64 size) __attr_no_discard;

/** `size` bytes to `size * 2` lower case hex plus a terminator, into `out` (which must hold that much). */
NYA_INTERNAL void _nya_plugin_hex_encode(const u8* data, u64 size, OUT char* out);

/**
 * Folds one file of the plugin into the running hash: its relative path, a separator, its length, then
 * its bytes. `required` decides whether an absent file is NYA_ERROR_NOT_FOUND or simply skipped.
 * */
NYA_INTERNAL NYA_Error _nya_plugin_digest_feed(NYA_CryptoSha256* sha256, NYA_ConstCString directory, NYA_ConstCString relative, b8 required)
    __attr_no_discard;

/** Byte order, shorter first on a shared prefix, so `src/` is hashed in an order an author can predict. */
NYA_INTERNAL s32 _nya_plugin_signature_name_compare(const NYA_String* a, const NYA_String* b) __attr_no_discard;

/** Copies the text after `"<key> "` on the line that starts with it into `out`, trimmed. False if absent. */
NYA_INTERNAL b8 _nya_plugin_signature_field(NYA_ConstCString text, NYA_ConstCString key, OUT char* out, u64 capacity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DIGEST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_digest(NYA_ConstCString directory, OUT NYA_CryptoSha256Digest* out_digest) {
    if (directory == nullptr || out_digest == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_digest needs a directory and somewhere to write");
    }

    *out_digest = (NYA_CryptoSha256Digest){ 0 };

    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);

    // The domain tag first, so this digest is this engine's and no plain SHA-256 of the same files.
    nya_crypto_sha256_update(&sha256, (const u8*)_NYA_PLUGIN_DIGEST_DOMAIN, sizeof(_NYA_PLUGIN_DIGEST_DOMAIN) - 1);

    // The manifest and the entry point, in this fixed order and both required: a directory without either
    // is not a plugin, and signing a digest of nothing would be a signature over nothing.
    NYA_TRY(_nya_plugin_digest_feed(&sha256, directory, NYA_PLUGIN_MANIFEST_FILE, true));
    NYA_TRY(_nya_plugin_digest_feed(&sha256, directory, NYA_PLUGIN_ENTRY_FILE, true));

    // Then every `.lua` under `src/`, in name order. The same enumeration nya_plugin_load runs them in, so the
    // signature covers exactly the code that executes and in the same order it is read.
    char sources[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(sources, sizeof(sources), "%s/%s", directory, NYA_PLUGIN_SOURCE_DIRECTORY);

    if (nya_filesystem_is_directory(sources)) {
        NYA_Arena* arena = nya_arena_create(.name = "plugin_digest");
        defer      nya_arena_destroy(arena);

        NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
        NYA_TRY(nya_filesystem_list(arena, sources, &entries));

        NYA_ArrayᐸNYA_Stringᐳ* names = nya_array_create(arena, NYA_String);

        nya_array_foreach (entries, entry) {
            if (entry->type != NYA_FILE_TYPE_FILE) continue;
            if (!nya_string_ends_with(entry->name, NYA_PLUGIN_SOURCE_EXTENSION)) continue;

            nya_array_push_back(names, *nya_string_clone(arena, entry->name));
        }

        nya_array_sort(names, _nya_plugin_signature_name_compare);

        nya_array_foreach (names, name) {
            char relative[NYA_PLUGIN_PATH_MAX];
            (void)snprintf(relative, sizeof(relative), "%s/%s", NYA_PLUGIN_SOURCE_DIRECTORY, nya_string_to_cstring(arena, name));

            NYA_TRY(_nya_plugin_digest_feed(&sha256, directory, relative, true));
        }
    }

    nya_crypto_sha256_end(&sha256, out_digest);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PINNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_trust_key(NYA_ConstCString publisher, const NYA_CryptoSignPublicKey* public_key) {
    if (publisher == nullptr || publisher[0] == '\0' || public_key == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_trust_key needs a publisher and a key");
    }

    // A second pin of the same publisher is a key rotation, not a new entry: replacing it in place keeps
    // one publisher to one key, so a stale key cannot linger beside its replacement and verify a plugin.
    _NYA_PluginTrustedKey* slot = nullptr;
    for (u32 i = 0; i < NYA_PLUGIN_TRUSTED_KEY_MAX; i++) {
        if (_nya_plugin_trusted_keys[i].used && nya_string_equals(_nya_plugin_trusted_keys[i].publisher, publisher)) {
            slot = &_nya_plugin_trusted_keys[i];
            break;
        }
    }

    if (slot == nullptr) {
        for (u32 i = 0; i < NYA_PLUGIN_TRUSTED_KEY_MAX; i++) {
            if (_nya_plugin_trusted_keys[i].used) continue;
            slot = &_nya_plugin_trusted_keys[i];
            break;
        }
    }

    if (slot == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "cannot pin '%s': " FMTu32 " publisher keys are already pinned", publisher,
                         (u32)NYA_PLUGIN_TRUSTED_KEY_MAX);
    }

    *slot = (_NYA_PluginTrustedKey){ .used = true, .key = *public_key };
    (void)snprintf(slot->publisher, sizeof(slot->publisher), "%s", publisher);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VERIFYING AND SIGNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_signature_verify(NYA_ConstCString directory, OUT char* out_publisher, u64 capacity) {
    if (directory == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_signature_verify needs a directory");
    if (out_publisher != nullptr && capacity > 0) out_publisher[0] = '\0';

    char path[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", directory, NYA_PLUGIN_SIGNATURE_FILE);

    if (!nya_filesystem_is_file(path)) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no %s", directory, NYA_PLUGIN_SIGNATURE_FILE);

    NYA_Arena* arena = nya_arena_create(.name = "plugin_signature");
    defer      nya_arena_destroy(arena);

    NYA_String* contents = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, contents));

    NYA_ConstCString text = nya_string_to_cstring(arena, contents);

    // The three fields the file carries. The publisher is a label; the two that matter are the key that
    // signed it and the signature itself, both hex.
    char publisher[NYA_PLUGIN_PUBLISHER_MAX];
    char key_hex[(NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2) + 1];
    char sig_hex[(NYA_CRYPTO_SIGNATURE_BYTES * 2) + 1];

    if (!_nya_plugin_signature_field(text, "publisher", publisher, sizeof(publisher)) ||
        !_nya_plugin_signature_field(text, "key", key_hex, sizeof(key_hex)) ||
        !_nya_plugin_signature_field(text, "sig", sig_hex, sizeof(sig_hex))) {
        return nya_error(NYA_ERROR_PARSE, "'%s' is not a plugin signature", path);
    }

    NYA_CryptoSignPublicKey signer    = { 0 };
    NYA_CryptoSignature      signature = { 0 };

    if (!_nya_plugin_hex_decode(key_hex, strlen(key_hex), signer.bytes, sizeof(signer.bytes)) ||
        !_nya_plugin_hex_decode(sig_hex, strlen(sig_hex), signature.bytes, sizeof(signature.bytes))) {
        return nya_error(NYA_ERROR_PARSE, "'%s' has a malformed key or signature", path);
    }

    // The signer's own claim, and worth nothing on its own: what makes the key trustworthy is that the
    // program pinned it, checked next. A plugin cannot become trusted by naming a publisher.
    const _NYA_PluginTrustedKey* pinned = nullptr;
    for (u32 i = 0; i < NYA_PLUGIN_TRUSTED_KEY_MAX; i++) {
        if (!_nya_plugin_trusted_keys[i].used) continue;
        if (nya_memcmp(_nya_plugin_trusted_keys[i].key.bytes, signer.bytes, sizeof(signer.bytes)) != 0) continue;

        pinned = &_nya_plugin_trusted_keys[i];
        break;
    }

    if (pinned == nullptr) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' is signed by '%s', whose key this program does not pin", directory, publisher);
    }

    NYA_CryptoSha256Digest digest = { 0 };
    NYA_TRY(nya_plugin_digest(directory, &digest));

    if (!nya_crypto_sign_verify(&pinned->key, digest.bytes, sizeof(digest.bytes), &signature)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' does not match its signature: its contents have changed since it was signed", directory);
    }

    // The pinned name, not the file's claim: the key is the identity, so the trustworthy label is the one
    // the program pinned the key under.
    if (out_publisher != nullptr && capacity > 0) (void)snprintf(out_publisher, capacity, "%s", pinned->publisher);

    return NYA_OK;
}

NYA_Error nya_plugin_signature_write(
    NYA_ConstCString               directory,
    NYA_ConstCString               publisher,
    const NYA_CryptoSignSecretKey* secret_key,
    const NYA_CryptoSignPublicKey* public_key
) {
    if (directory == nullptr || publisher == nullptr || publisher[0] == '\0' || secret_key == nullptr || public_key == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_signature_write needs a directory, a publisher and a key pair");
    }

    NYA_CryptoSha256Digest digest = { 0 };
    NYA_TRY(nya_plugin_digest(directory, &digest));

    NYA_CryptoSignature signature = { 0 };
    nya_crypto_sign(secret_key, digest.bytes, sizeof(digest.bytes), &signature);

    char key_hex[(NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2) + 1];
    char sig_hex[(NYA_CRYPTO_SIGNATURE_BYTES * 2) + 1];
    _nya_plugin_hex_encode(public_key->bytes, sizeof(public_key->bytes), key_hex);
    _nya_plugin_hex_encode(signature.bytes, sizeof(signature.bytes), sig_hex);

    NYA_Arena* arena = nya_arena_create(.name = "plugin_signature_write");
    defer      nya_arena_destroy(arena);

    NYA_String* out = nya_string_sprintf(arena, "%s\npublisher %s\nkey %s\nsig %s\n", _NYA_PLUGIN_SIGNATURE_MAGIC, publisher, key_hex, sig_hex);

    char path[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", directory, NYA_PLUGIN_SIGNATURE_FILE);

    return nya_file_write_atomic(path, out);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 _nya_plugin_hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return (character - 'a') + 10;
    if (character >= 'A' && character <= 'F') return (character - 'A') + 10;

    return -1;
}

b8 _nya_plugin_hex_decode(NYA_ConstCString hex, u64 hex_length, OUT u8* out, u64 size) {
    nya_assert(hex != nullptr && out != nullptr);

    if (hex_length != size * 2) return false;

    for (u64 i = 0; i < size; i++) {
        s32 high = _nya_plugin_hex_value(hex[i * 2]);
        s32 low  = _nya_plugin_hex_value(hex[(i * 2) + 1]);

        if (high < 0 || low < 0) return false;

        out[i] = (u8)((high << 4) | low);
    }

    return true;
}

void _nya_plugin_hex_encode(const u8* data, u64 size, OUT char* out) {
    nya_assert(data != nullptr && out != nullptr);

    static const char DIGITS[] = "0123456789abcdef";

    for (u64 i = 0; i < size; i++) {
        out[i * 2]       = DIGITS[(data[i] >> 4) & 0x0F];
        out[(i * 2) + 1] = DIGITS[data[i] & 0x0F];
    }

    out[size * 2] = '\0';
}

NYA_Error _nya_plugin_digest_feed(NYA_CryptoSha256* sha256, NYA_ConstCString directory, NYA_ConstCString relative, b8 required) {
    nya_assert(sha256 != nullptr && directory != nullptr && relative != nullptr);

    char path[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", directory, relative);

    if (!nya_filesystem_is_file(path)) {
        if (required) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no %s", directory, relative);
        return NYA_OK;
    }

    NYA_Arena* arena = nya_arena_create(.name = "plugin_digest_file");
    defer      nya_arena_destroy(arena);

    NYA_String* contents = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, contents));

    // The relative path, a NUL, the length, then the bytes. The path binds a file to its place, and the
    // length between path and bytes is what stops two files being read as one differently split.
    nya_crypto_sha256_update(sha256, (const u8*)relative, strlen(relative));

    u8 separator = 0;
    nya_crypto_sha256_update(sha256, &separator, 1);

    u8 length[8];
    for (u32 i = 0; i < 8; i++) length[i] = (u8)((contents->length >> (i * 8)) & 0xFF);
    nya_crypto_sha256_update(sha256, length, sizeof(length));

    nya_crypto_sha256_update(sha256, contents->items, contents->length);

    return NYA_OK;
}

s32 _nya_plugin_signature_name_compare(const NYA_String* a, const NYA_String* b) {
    nya_assert(a != nullptr && b != nullptr);

    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);

    if (difference != 0) return difference < 0 ? -1 : 1;
    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

b8 _nya_plugin_signature_field(NYA_ConstCString text, NYA_ConstCString key, OUT char* out, u64 capacity) {
    nya_assert(text != nullptr && key != nullptr && out != nullptr && capacity > 0);

    out[0]      = '\0';
    u64 key_len = strlen(key);

    for (NYA_ConstCString line = text; line != nullptr && *line != '\0';) {
        NYA_ConstCString newline = strchr(line, '\n');
        u64              line_len = newline != nullptr ? (u64)(newline - line) : strlen(line);

        // "<key> ...": the key, then a single space, then the value that runs to the line end.
        if (line_len > key_len + 1 && nya_memcmp(line, key, key_len) == 0 && line[key_len] == ' ') {
            NYA_ConstCString value     = line + key_len + 1;
            u64              value_len = line_len - key_len - 1;

            // Trim a trailing carriage return, so a file written on Windows reads the same.
            if (value_len > 0 && value[value_len - 1] == '\r') value_len--;

            if (value_len >= capacity) return false;

            nya_memcpy(out, value, value_len);
            out[value_len] = '\0';

            return true;
        }

        line = newline != nullptr ? newline + 1 : nullptr;
    }

    return false;
}

#ifdef NYA_TESTING
__attr_maybe_unused void _nya_plugin_trusted_keys_reset(void) {
    for (u32 i = 0; i < NYA_PLUGIN_TRUSTED_KEY_MAX; i++) _nya_plugin_trusted_keys[i] = (_NYA_PluginTrustedKey){ 0 };
}
#endif
