/**
 * @file plugin_signer.c
 *
 * The developer side of signed plugins, as a small program: make a signing key, and sign a plugin with
 * it. It is a program of its own rather than a `./build` subcommand because signing needs the engine's
 * Ed25519, and the build tool is compiled without the crypto module (it hashes nothing). `./build plugin`
 * compiles and runs this; a developer never invokes it by hand.
 *
 *   plugin_signer keygen [--seed <file>]                       draw a key, write its seed, print the public key to pin
 *   plugin_signer sign <directory> --seed <file> [--publisher <name>]  write <directory>/plugin.sig
 *
 * The only private key involved is the seed in the file named by `--seed`; it never enters a binary, and
 * this program prints only the public half. See src/nyangine/core/core_plugin_signature.h.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Where keygen writes its seed and sign reads one, unless --seed says otherwise. */
#define PLUGIN_SEED_DEFAULT "plugin_signing.seed"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HEX
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static s32 hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return (character - 'a') + 10;
    if (character >= 'A' && character <= 'F') return (character - 'A') + 10;

    return -1;
}

static b8 hex_decode(NYA_ConstCString hex, u64 hex_length, OUT u8* out, u64 size) {
    if (hex_length != size * 2) return false;

    for (u64 i = 0; i < size; i++) {
        s32 high = hex_value(hex[i * 2]);
        s32 low  = hex_value(hex[(i * 2) + 1]);

        if (high < 0 || low < 0) return false;

        out[i] = (u8)((high << 4) | low);
    }

    return true;
}

static void hex_encode(const u8* data, u64 size, OUT char* out) {
    static const char DIGITS[] = "0123456789abcdef";

    for (u64 i = 0; i < size; i++) {
        out[i * 2]       = DIGITS[(data[i] >> 4) & 0x0F];
        out[(i * 2) + 1] = DIGITS[data[i] & 0x0F];
    }

    out[size * 2] = '\0';
}

/** The value after `--<name>` in argv, or nullptr. A tiny parser: this program's arguments are few. */
static NYA_ConstCString argument_value(s32 argc, NYA_CString argv[], NYA_ConstCString name) {
    for (s32 i = 0; i + 1 < argc; i++) {
        if (nya_string_equals(argv[i], name)) return argv[i + 1];
    }

    return nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMMANDS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static s32 keygen(s32 argc, NYA_CString argv[]) {
    NYA_ConstCString seed_path = argument_value(argc, argv, "--seed");
    if (seed_path == nullptr) seed_path = PLUGIN_SEED_DEFAULT;

    // Never over a key that already exists: overwriting a seed is losing the identity every plugin signed
    // with it had, silently.
    if (nya_filesystem_exists(seed_path)) {
        (void)fprintf(stderr, "Error: '%s' already exists; refusing to overwrite a signing key.\n", seed_path);
        return EXIT_FAILURE;
    }

    NYA_Arena* arena = nya_arena_create(.name = "keygen");
    defer      nya_arena_destroy(arena);

    NYA_CryptoSignKeyPair pair = { 0 };
    if (!nya_crypto_sign_key_pair_create(&pair).ok) {
        (void)fprintf(stderr, "Error: could not draw a key pair from the system random source.\n");
        return EXIT_FAILURE;
    }
    defer nya_crypto_sign_key_pair_destroy(&pair);

    // The seed is the first 32 bytes of the secret key; the public key is derived from it, so the seed
    // alone is the whole private key and all that must be kept.
    char seed_hex[(NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2) + 1];
    char key_hex[(NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2) + 1];
    hex_encode(pair.secret_key.bytes, NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES, seed_hex);
    hex_encode(pair.public_key.bytes, sizeof(pair.public_key.bytes), key_hex);

    NYA_String* seed_file = nya_string_sprintf(arena, "%s\n", seed_hex);
    if (!nya_file_write_atomic(seed_path, seed_file).ok) {
        (void)fprintf(stderr, "Error: could not write the seed to '%s'.\n", seed_path);
        return EXIT_FAILURE;
    }

    // Read and written by its owner only: a signing seed other users can read is a signing seed they have.
    if (nya_os_file_mode_set(seed_path, 0o600) != NYA_OS_FILE_STATUS_OK) {
        (void)fprintf(stderr, "Warning: could not restrict permissions on '%s'; make sure only you can read it.\n", seed_path);
    }

    // The seed never touches stdout. The public key does: it is what a program pins to trust this key.
    printf("Wrote a new signing key to %s (keep it secret).\n", seed_path);
    printf("Public key: %s\n", key_hex);
    printf("Pin it in the program from the host's own startup, before nya_plugin_load_all:\n");
    printf("    nya_plugin_trust_key(\"<publisher>\", &(NYA_CryptoSignPublicKey){ .bytes = { /* %s */ } });\n", key_hex);

    return EXIT_SUCCESS;
}

static s32 sign(s32 argc, NYA_CString argv[]) {
    // The directory is the first argument that is not a flag or a flag's value.
    NYA_ConstCString directory = nullptr;
    for (s32 i = 0; i < argc; i++) {
        if (argv[i][0] == '-') {
            i++;  // skip the flag's value
            continue;
        }
        directory = argv[i];
        break;
    }

    if (directory == nullptr) {
        (void)fprintf(stderr, "Error: no plugin directory. Usage: plugin_signer sign <directory> --seed <file>\n");
        return EXIT_FAILURE;
    }

    NYA_ConstCString seed_path = argument_value(argc, argv, "--seed");
    if (seed_path == nullptr) seed_path = PLUGIN_SEED_DEFAULT;

    NYA_ConstCString publisher = argument_value(argc, argv, "--publisher");

    NYA_Arena* arena = nya_arena_create(.name = "sign");
    defer      nya_arena_destroy(arena);

    if (!nya_filesystem_is_file(seed_path)) {
        (void)fprintf(stderr, "Error: no signing key at '%s'. Make one with 'plugin keygen'.\n", seed_path);
        return EXIT_FAILURE;
    }

    NYA_String* seed_contents = nya_string_create(arena);
    if (!nya_file_read(seed_path, seed_contents).ok) {
        (void)fprintf(stderr, "Error: could not read the seed from '%s'.\n", seed_path);
        return EXIT_FAILURE;
    }

    // Trim trailing whitespace (the newline keygen wrote, or one an editor added).
    u64 seed_length = seed_contents->length;
    while (seed_length > 0 && (seed_contents->items[seed_length - 1] == '\n' || seed_contents->items[seed_length - 1] == '\r' ||
                               seed_contents->items[seed_length - 1] == ' ' || seed_contents->items[seed_length - 1] == '\t')) {
        seed_length--;
    }

    NYA_CryptoKey32 seed = { 0 };
    defer           nya_crypto_key_destroy(&seed);

    if (!hex_decode((NYA_ConstCString)seed_contents->items, seed_length, seed.bytes, sizeof(seed.bytes))) {
        (void)fprintf(stderr, "Error: '%s' is not %d hex characters of seed.\n", seed_path, NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2);
        return EXIT_FAILURE;
    }

    NYA_CryptoSignKeyPair pair = { 0 };
    nya_crypto_sign_key_pair_from_seed(&seed, &pair);
    defer nya_crypto_sign_key_pair_destroy(&pair);

    // The manifest names the author; the signature's publisher defaults to it, so one plugin carries one
    // name in both places unless the developer overrides it.
    NYA_PluginManifest manifest = { 0 };
    if (publisher == nullptr && nya_plugin_manifest_load(directory, &manifest).ok && manifest.author[0] != '\0') {
        publisher = nya_string_to_cstring(arena, nya_string_from(arena, manifest.author));
    }

    if (publisher == nullptr || publisher[0] == '\0') {
        (void)fprintf(stderr, "Error: no publisher. The manifest names no author; pass --publisher <name>.\n");
        return EXIT_FAILURE;
    }

    NYA_Error written = nya_plugin_signature_write(directory, publisher, &pair.secret_key, &pair.public_key);
    if (!written.ok) {
        (void)fprintf(stderr, "Error: could not sign '%s': %s\n", directory, (NYA_ConstCString)written.message);
        return EXIT_FAILURE;
    }

    char key_hex[(NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES * 2) + 1];
    hex_encode(pair.public_key.bytes, sizeof(pair.public_key.bytes), key_hex);

    printf("Signed %s as '%s'.\n", directory, publisher);
    printf("Its %s verifies against the pinned key %s.\n", NYA_PLUGIN_SIGNATURE_FILE, key_hex);

    return EXIT_SUCCESS;
}

s32 main(s32 argc, NYA_CString argv[]) {
    if (argc < 2) {
        (void)fprintf(stderr, "Usage: plugin_signer <keygen|sign> ...\n");
        return EXIT_FAILURE;
    }

    if (nya_string_equals(argv[1], "keygen")) return keygen(argc - 2, argv + 2);
    if (nya_string_equals(argv[1], "sign")) return sign(argc - 2, argv + 2);

    (void)fprintf(stderr, "Error: unknown command '%s'. Use keygen or sign.\n", argv[1]);
    return EXIT_FAILURE;
}
