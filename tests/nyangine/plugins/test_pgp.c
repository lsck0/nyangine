/**
 * OpenPGP through gpg: a real key generated here, a real message encrypted to it, and a real
 * decryption proving the person holding the private key is the only one who can read it.
 *
 * The key is made by this test rather than checked in, because a key in a repository is a key
 * somebody eventually uses for something. Everything runs in a temporary directory of its own, and a
 * machine without gpg skips the round trip and still checks what this answers without one.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** What the server would encrypt: the sentence that says what is being approved, and the code. */
#define MESSAGE "nyangine wants to let ada@example.test in from 203.0.113.9. The code is 7F3K-92QX."

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_pgp");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what this answers on a machine with no gpg, which is most containers.
  // ─────────────────────────────────────────────────────────────────────────────
  if (!nya_pgp_available()) {
    NYA_String* armored = nullptr;
    NYA_Error   refused = nya_pgp_encrypt(arena, "not a key", (const u8*)MESSAGE, strlen(MESSAGE), &armored);

    nya_check(!refused.ok, "without gpg there is nothing to encrypt with");
    nya_check(refused.kind == NYA_ERROR_NOT_SUPPORTED, "and it says so as unsupported rather than as a failure, got %d", (s32)refused.kind);
    nya_check(armored == nullptr, "with nothing handed back");

    printf("  gpg is not installed here; the round trip is skipped\n");

    return nya_check_failures() == 0 ? 0 : 1;
  }

  printf("  %s\n", nya_pgp_version());

  /*
   * A key of this test's own, in a home of this test's own. Generated unattended, which is what
   * `%no-protection` is for: a passphrase would need a pinentry and a person.
   */
  NYA_String* temporary = nullptr;
  nya_check(nya_filesystem_temp_directory(arena, &temporary).ok, "there is a temporary directory");

  NYA_String*      home_string = nya_string_sprintf(arena, "%s/nyangine-test-pgp", nya_string_to_cstring(arena, temporary));
  NYA_ConstCString home        = nya_string_to_cstring(arena, home_string);

  (void)nya_filesystem_delete_recursive(home);
  nya_check(nya_filesystem_create_directory(home).ok, "and a home for gpg under it");

  defer (void)nya_filesystem_delete_recursive(home);

  NYA_ConstCString recipe_path = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/recipe", home));
  NYA_ConstCString key_path    = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/public.asc", home));

  static const char RECIPE[] = "%no-protection\n"
                               "Key-Type: eddsa\n"
                               "Key-Curve: ed25519\n"
                               "Subkey-Type: ecdh\n"
                               "Subkey-Curve: cv25519\n"
                               "Name-Real: Nyangine Test Fixture\n"
                               "Name-Email: fixture@example.test\n"
                               "Expire-Date: 0\n"
                               "%commit\n";

  {
    NYA_File recipe = { 0 };
    nya_check(nya_file_open(recipe_path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_CREATE | NYA_FILE_MODE_TRUNCATE, &recipe).ok, "the recipe is written");
    nya_check(nya_file_write_bytes(&recipe, (const u8*)RECIPE, sizeof(RECIPE) - 1).ok, "in full");
    nya_file_close(&recipe);
  }

  {
    NYA_Command generate = {
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .arena     = arena,
      .program   = "gpg",
      .arguments = { "--batch", "--no-tty", "--homedir", home, "--generate-key", recipe_path, nullptr },
    };

    nya_check(nya_command_run(&generate).ok && generate.exit_code == 0, "a key is generated: %s",
              generate.stderr_content != nullptr ? nya_string_to_cstring(arena, generate.stderr_content) : "");
    nya_command_destroy(&generate);
  }

  {
    NYA_Command export_key = {
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .arena     = arena,
      .program   = "gpg",
      .arguments = { "--batch", "--no-tty", "--homedir", home, "--armor", "--output", key_path, "--export", "fixture@example.test", nullptr },
    };

    nya_check(nya_command_run(&export_key).ok && export_key.exit_code == 0, "and exported as an armored public key");
    nya_command_destroy(&export_key);
  }

  NYA_String* public_key = nullptr;
  {
    NYA_File file = { 0 };
    nya_check(nya_file_open(key_path, NYA_FILE_MODE_READ, &file).ok, "the public key reads back");

    public_key = nya_string_create(arena);

    u8  buffer[1024] = { 0 };
    u64 read         = 0;

    while (nya_file_read_bytes(&file, buffer, sizeof(buffer), &read).ok && read > 0) {
      for (u64 index = 0; index < read; index++) nya_string_push_back(public_key, buffer[index]);
    }

    nya_file_close(&file);
  }

  NYA_ConstCString recipient = nya_string_to_cstring(arena, public_key);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the fingerprint, which is what a person checks and an account stores.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String* fingerprint = nullptr;
    nya_check(nya_pgp_fingerprint(arena, recipient, &fingerprint).ok, "the key's fingerprint is read");

    NYA_ConstCString text = nya_string_to_cstring(arena, fingerprint);

    nya_check(strlen(text) == 40, "as forty hex characters, got '%s'", text);

    for (u64 index = 0; text[index] != '\0'; index++) {
      b8 hex = (text[index] >= '0' && text[index] <= '9') || (text[index] >= 'A' && text[index] <= 'F');
      nya_check(hex, "and nothing but hex, got '%s'", text);
    }

    NYA_String* refused = nullptr;
    nya_check(!nya_pgp_fingerprint(arena, "-----BEGIN PGP PUBLIC KEY BLOCK-----\nnot a key\n-----END PGP PUBLIC KEY BLOCK-----\n", &refused).ok,
              "something shaped like a key but not one is refused");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the round trip, which is the only thing that proves any of this works.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String* armored = nullptr;
    nya_check(nya_pgp_encrypt(arena, recipient, (const u8*)MESSAGE, strlen(MESSAGE), &armored).ok, "the message encrypts to that key");

    NYA_ConstCString ciphertext = nya_string_to_cstring(arena, armored);

    nya_check(nya_string_contains(ciphertext, "BEGIN PGP MESSAGE"), "and comes back armored, got '%.40s'", ciphertext);
    nya_check(!nya_string_contains(ciphertext, "7F3K-92QX"), "with the code nowhere in it");

    // and only the holder of the private key can read it, which is the whole point.
    NYA_ConstCString cipher_path = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/message.asc", home));

    {
      NYA_File file = { 0 };
      nya_check(nya_file_open(cipher_path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_CREATE | NYA_FILE_MODE_TRUNCATE, &file).ok, "the ciphertext is written");
      nya_check(nya_file_write_bytes(&file, armored->items, armored->length).ok, "in full");
      nya_file_close(&file);
    }

    NYA_Command decrypt = {
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .arena     = arena,
      .program   = "gpg",
      .arguments = { "--batch", "--no-tty", "--homedir", home, "--decrypt", cipher_path, nullptr },
    };

    nya_check(nya_command_run(&decrypt).ok && decrypt.exit_code == 0, "the key's owner decrypts it");

    NYA_ConstCString plaintext = nya_string_to_cstring(arena, decrypt.stdout_content);

    nya_check(nya_string_equals(plaintext, MESSAGE), "and gets back exactly what was sent, got '%s'", plaintext);

    nya_command_destroy(&decrypt);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what is refused before gpg is ever started.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String* armored = nullptr;

    nya_check(!nya_pgp_encrypt(arena, "", (const u8*)MESSAGE, strlen(MESSAGE), &armored).ok, "a missing recipient key is refused");
    nya_check(!nya_pgp_encrypt(arena, recipient, (const u8*)MESSAGE, 0, &armored).ok, "and an empty message");

    u8 enormous[NYA_PGP_MAX_MESSAGE_BYTES + 1] = { 0 };
    nya_memset(enormous, 'x', sizeof(enormous));

    nya_check(!nya_pgp_encrypt(arena, recipient, enormous, sizeof(enormous), &armored).ok, "and one past what this encrypts");

    // a key gpg cannot read is the case a user's profile form actually produces.
    NYA_Error refused = nya_pgp_encrypt(arena, "this is not a key at all", (const u8*)MESSAGE, strlen(MESSAGE), &armored);

    nya_check(!refused.ok, "and something that is not a key");
    nya_check(refused.message[0] != '\0', "with what gpg said about it: %s", (NYA_ConstCString)refused.message);
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
