#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_command.h"
#include "nyangine-std/base/base_filesystem.h"
#include "nyangine-std/base/base_preflight.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-std/os/os_random.h"
#include "nyangine-plugins/pgp/pgp.h"

// ───────────────────────────────────── PRIVATE TYPES ─────────────────────────────────────

/** What `gpg --version` said, asked once. */
typedef struct {
    b8   asked;
    b8   available;
    char version[64];
} _NYA_PgpState;

NYA_INTERNAL _NYA_PgpState _NYA_PGP = { 0 };

/** Bytes of the stderr gpg wrote that an error quotes back. Enough for its first sentence. */
#define _NYA_PGP_MAX_DETAIL 256

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/**
 * A directory of this call's own, under the system temporary directory, with a random name.
 *
 * gpg is given it as its `--homedir`, so nothing here reads, writes or locks the keyring of whatever
 * user this process runs as — and two calls at once cannot meet.
 * */
NYA_INTERNAL NYA_Error _nya_pgp_workspace(NYA_Arena* arena, OUT NYA_String** out_path) __attr_no_discard;

/** Writes `size` bytes to `path`, creating it. */
NYA_INTERNAL NYA_Error _nya_pgp_write(NYA_ConstCString path, const u8* data, u64 size) __attr_no_discard;

/** Reads a whole file into a string, up to `limit` bytes. */
NYA_INTERNAL NYA_Error _nya_pgp_read(NYA_Arena* arena, NYA_ConstCString path, u64 limit, OUT NYA_String** out_contents) __attr_no_discard;

/** Overwrites a file's bytes before it is deleted, so a code does not outlive the call in a temporary file. */
NYA_INTERNAL void _nya_pgp_wipe(NYA_ConstCString path);

/** The first line of what gpg wrote to stderr, for an error message. Never more than one line. */
NYA_INTERNAL void _nya_pgp_detail(const NYA_Command* command, OUT char* out_detail, u64 capacity);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

b8 nya_pgp_available(void) {
    if (_NYA_PGP.asked) return _NYA_PGP.available;

    _NYA_PGP.asked = true;

    NYA_Arena* arena = nya_arena_create(.name = "pgp_probe");
    defer nya_arena_destroy(arena);

    NYA_Command command = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .arena     = arena,
        .program   = "gpg",
        .arguments = { "--version", nullptr },
    };

    NYA_Error ran = nya_command_run(&command);
    defer nya_command_destroy(&command);

    // Not an error worth reporting: a machine without gpg is a machine that does not offer this, and
    // the caller asked precisely so it could find that out quietly.
    if (!ran.ok || command.exit_code != 0) return false;

    // "gpg (GnuPG) 2.4.9" is the first line, and its last word is the version.
    NYA_ConstCString text = nya_string_to_cstring(arena, command.stdout_content);

    for (u64 index = 0; text[index] != '\0' && text[index] != '\n' && index < sizeof(_NYA_PGP.version) - 1; index++) {
        _NYA_PGP.version[index]     = text[index];
        _NYA_PGP.version[index + 1] = '\0';
    }

    _NYA_PGP.available = true;

    return true;
}

NYA_ConstCString nya_pgp_version(void) {
    (void)nya_pgp_available();

    return _NYA_PGP.version;
}

void nya_pgp_require(void) {
    nya_require_program("gpg", "the PGP second-factor login flow (encrypting a one-time code to a user's public key)");
}

NYA_Error nya_pgp_encrypt(NYA_Arena* arena, NYA_ConstCString recipient_key, const u8* message, u64 message_size, NYA_String** out_armored) {
    nya_assert(arena != nullptr && out_armored != nullptr);

    *out_armored = nullptr;

    if (!nya_pgp_available()) return nya_error(NYA_ERROR_NOT_SUPPORTED, "this machine has no gpg, so nothing can be encrypted to a public key");

    if (recipient_key == nullptr || recipient_key[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "there is no recipient key to encrypt to");
    if (strlen(recipient_key) >= NYA_PGP_MAX_KEY_BYTES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is larger than a public key this accepts");

    if (message == nullptr || message_size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "there is no message to encrypt");
    if (message_size > NYA_PGP_MAX_MESSAGE_BYTES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is longer than the short message this encrypts");

    NYA_String* workspace = nullptr;
    NYA_TRY(_nya_pgp_workspace(arena, &workspace));

    NYA_ConstCString home = nya_string_to_cstring(arena, workspace);

    NYA_ConstCString key_path        = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/recipient.asc", home));
    NYA_ConstCString plaintext_path  = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/message", home));
    NYA_ConstCString ciphertext_path = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/message.asc", home));

    // The whole directory goes however this ends, and the plaintext is overwritten first: a one-time code in a temporary file is worth more than the ciphertext.
    defer {
        _nya_pgp_wipe(plaintext_path);
        (void)nya_filesystem_delete_recursive(home);
    }

    NYA_TRY(_nya_pgp_write(key_path, (const u8*)recipient_key, strlen(recipient_key)));
    NYA_TRY(_nya_pgp_write(plaintext_path, message, message_size));

    NYA_Command command = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .arena     = arena,
        .program   = "gpg",
        .arguments = {
            "--batch", "--yes", "--no-tty",

            // Its own home, so the server's keyring is neither read nor locked.
            "--homedir", home,

            // Key used from the file, never imported, so a user changing their key changes nothing here; trust `always` because the caller already trusted it by storing it.
            "--recipient-file", key_path,
            "--trust-model", "always",

            "--armor", "--output", ciphertext_path,
            "--encrypt", plaintext_path,
            nullptr,
        },
    };

    NYA_Error ran = nya_command_run(&command);
    defer nya_command_destroy(&command);

    if (!ran.ok) return nya_error(NYA_ERROR_IO, "gpg could not be run: %s", (NYA_ConstCString)ran.message);

    if (command.exit_code != 0) {
        char detail[_NYA_PGP_MAX_DETAIL] = { 0 };
        _nya_pgp_detail(&command, detail, sizeof(detail));

        return nya_error(NYA_ERROR_NOT_OK, "gpg refused the recipient key: %s", detail);
    }

    NYA_TRY(_nya_pgp_read(arena, ciphertext_path, NYA_PGP_MAX_KEY_BYTES, out_armored));

    return NYA_OK;
}

NYA_Error nya_pgp_fingerprint(NYA_Arena* arena, NYA_ConstCString recipient_key, NYA_String** out_fingerprint) {
    nya_assert(arena != nullptr && out_fingerprint != nullptr);

    *out_fingerprint = nullptr;

    if (!nya_pgp_available()) return nya_error(NYA_ERROR_NOT_SUPPORTED, "this machine has no gpg, so a key cannot be read");

    if (recipient_key == nullptr || recipient_key[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "there is no key to read");
    if (strlen(recipient_key) >= NYA_PGP_MAX_KEY_BYTES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is larger than a public key this accepts");

    NYA_String* workspace = nullptr;
    NYA_TRY(_nya_pgp_workspace(arena, &workspace));

    NYA_ConstCString home     = nya_string_to_cstring(arena, workspace);
    NYA_ConstCString key_path = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/recipient.asc", home));

    defer (void)nya_filesystem_delete_recursive(home);

    NYA_TRY(_nya_pgp_write(key_path, (const u8*)recipient_key, strlen(recipient_key)));

    NYA_Command command = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .arena     = arena,
        .program   = "gpg",
        .arguments = {
            "--batch", "--no-tty",
            "--homedir", home,

            // The machine-readable listing; the human one is formatted for a terminal and gets reformatted.
            "--with-colons", "--with-fingerprint",
            "--show-keys", key_path,
            nullptr,
        },
    };

    NYA_Error ran = nya_command_run(&command);
    defer nya_command_destroy(&command);

    if (!ran.ok) return nya_error(NYA_ERROR_IO, "gpg could not be run: %s", (NYA_ConstCString)ran.message);

    if (command.exit_code != 0) {
        char detail[_NYA_PGP_MAX_DETAIL] = { 0 };
        _nya_pgp_detail(&command, detail, sizeof(detail));

        return nya_error(NYA_ERROR_PARSE, "gpg does not read that as a public key: %s", detail);
    }

    // The colon listing is one record per line; `fpr` carries the fingerprint in its tenth field, and the first one is the primary key's, which is what to store.
    NYA_ConstCString text = nya_string_to_cstring(arena, command.stdout_content);

    for (u64 index = 0; text[index] != '\0';) {
        u64 end = index;
        while (text[end] != '\0' && text[end] != '\n') end++;

        if (strncmp(text + index, "fpr:", 4) == 0) {
            u64 field = 0;
            u64 at    = index;

            while (at < end && field < 9) {
                if (text[at] == ':') field++;
                at++;
            }

            u64 stop = at;
            while (stop < end && text[stop] != ':') stop++;

            if (stop > at) {
                *out_fingerprint = nya_string_create(arena);

                for (u64 byte = at; byte < stop; byte++) nya_string_push_back(*out_fingerprint, (u8)text[byte]);

                return NYA_OK;
            }
        }

        index = text[end] == '\0' ? end : end + 1;
    }

    return nya_error(NYA_ERROR_PARSE, "gpg read that key but reported no fingerprint for it");
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_pgp_workspace(NYA_Arena* arena, NYA_String** out_path) {
    NYA_String* temporary = nullptr;
    NYA_TRY(nya_filesystem_temp_directory(arena, &temporary));

    u8 bits[8] = { 0 };
    if (!nya_os_random_bytes(bits, sizeof(bits))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    char name[17] = { 0 };
    for (u64 index = 0; index < sizeof(bits); index++) (void)snprintf(name + (index * 2), 3, "%02x", bits[index]);

    *out_path = nya_string_sprintf(arena, "%s/nyangine-pgp-%s", nya_string_to_cstring(arena, temporary), name);

    NYA_ConstCString path = nya_string_to_cstring(arena, *out_path);

    NYA_TRY(nya_filesystem_create_directory(path));

    return NYA_OK;
}

NYA_Error _nya_pgp_write(NYA_ConstCString path, const u8* data, u64 size) {
    NYA_File file = { 0 };

    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_CREATE | NYA_FILE_MODE_TRUNCATE, &file));
    defer nya_file_close(&file);

    NYA_TRY(nya_file_write_bytes(&file, data, size));

    return NYA_OK;
}

NYA_Error _nya_pgp_read(NYA_Arena* arena, NYA_ConstCString path, u64 limit, NYA_String** out_contents) {
    *out_contents = nya_string_create(arena);

    NYA_File file = { 0 };

    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_READ, &file));
    defer nya_file_close(&file);

    u8  buffer[1024] = { 0 };
    u64 total        = 0;

    for (;;) {
        u64 read = 0;
        NYA_TRY(nya_file_read_bytes(&file, buffer, sizeof(buffer), &read));

        if (read == 0) break;

        total += read;
        if (total > limit) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "gpg wrote more than this reads back");

        for (u64 index = 0; index < read; index++) nya_string_push_back(*out_contents, buffer[index]);
    }

    return NYA_OK;
}

void _nya_pgp_wipe(NYA_ConstCString path) {
    u64 size = 0;
    if (!nya_filesystem_size(path, &size).ok || size == 0) return;

    NYA_File file = { 0 };
    if (!nya_file_open(path, NYA_FILE_MODE_WRITE, &file).ok) return;

    defer nya_file_close(&file);

    // One pass of zeroes: not to defeat a block-copying filesystem, but so the bytes are not left in a file somebody forgot to delete, which is the real failure.
    u8 zeroes[256] = { 0 };

    for (u64 written = 0; written < size; written += sizeof(zeroes)) {
        u64 chunk = size - written < sizeof(zeroes) ? size - written : sizeof(zeroes);

        if (!nya_file_write_bytes(&file, zeroes, chunk).ok) return;
    }

    (void)nya_file_flush(&file);
}

void _nya_pgp_detail(const NYA_Command* command, char* out_detail, u64 capacity) {
    out_detail[0] = '\0';

    if (command->stderr_content == nullptr || command->stderr_content->length == 0) {
        (void)snprintf(out_detail, capacity, "it said nothing and exited %d", command->exit_code);
        return;
    }

    // gpg's last line says what went wrong; taken whole and bounded rather than searched for a phrase, since those are translated.
    NYA_ConstCString text = (NYA_ConstCString)command->stderr_content->items;
    u64              size = command->stderr_content->length;

    while (size > 0 && (text[size - 1] == '\n' || text[size - 1] == '\r')) size--;

    u64 start = size;
    while (start > 0 && text[start - 1] != '\n') start--;

    u64 length = size - start;
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(out_detail, text + start, length);
    out_detail[length] = '\0';
}
