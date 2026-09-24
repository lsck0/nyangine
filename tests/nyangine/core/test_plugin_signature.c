/**
 * Signed plugins: core_plugin_signature.h. A plugin's digest is one stable value over its code; a
 * signature by a pinned publisher key verifies and any change, wrong key or missing signature is
 * refused; and nya_plugin_load turns that into a refusal before a line of an untrusted plugin runs.
 *
 * The keys here are drawn fresh from the crypto API, and no plugin is ever run — a signature is proved
 * against a pinned key, which is the whole property, without needing a VM or a real plugin on disk past
 * the few files these helpers write.
 **/

/*
 * Before nyangine.h: the host must discover this test's own tree, and defining a macro is also what
 * makes the test compile its own copy of the engine (see _test_shares_engine in src/build/test.c) so
 * NYA_PLUGIN_REQUIRE_SIGNATURE below is this suite's, not the shared build's.
 */
#define NYA_PLUGIN_DIRECTORY TEST_SIGNATURE_ROOT

#define TEST_SIGNATURE_ROOT "./.test_plugin_signature"

/* The point of the suite: the default posture. An unsigned plugin is refused, and only a signature by a
 * pinned key gets one loaded. */
#define NYA_PLUGIN_REQUIRE_SIGNATURE true

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FIXTURES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Writes `contents` to `<root>/<name>/<relative>`, creating every directory on the way. */
static void plugin_file_write(NYA_ConstCString name, NYA_ConstCString relative, NYA_ConstCString contents) {
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s/%s", TEST_SIGNATURE_ROOT, name, relative);

    char parent[512];
    (void)snprintf(parent, sizeof(parent), "%s", path);
    for (u64 i = strlen(parent); i > 0; i--) {
        if (parent[i - 1] != '/') continue;
        parent[i - 1] = '\0';
        break;
    }

    NYA_EXPECT(nya_filesystem_create_directory(parent), "while creating %s", parent);
    NYA_EXPECT(nya_file_write(path, contents), "while writing %s", path);
}

/** A minimal, valid plugin `name`: a manifest that names it, an entry point, and one file under src/. */
static void plugin_write(NYA_ConstCString name) {
    char manifest[1024];
    (void)snprintf(manifest, sizeof(manifest),
                   "nya 2 0\n"
                   "{\n"
                   "    name: string \"%s\";\n"
                   "    version: string \"1.0.0\";\n"
                   "    author: string \"a test\";\n"
                   "}\n",
                   name);

    plugin_file_write(name, NYA_PLUGIN_MANIFEST_FILE, manifest);
    plugin_file_write(name, NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");
    plugin_file_write(name, NYA_PLUGIN_SOURCE_DIRECTORY "/one.lua", "X = 1\n");
}

/** `<root>/<name>`, the directory the signature calls live on. */
static NYA_ConstCString plugin_directory(NYA_Arena* arena, NYA_ConstCString name) {
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", TEST_SIGNATURE_ROOT, name));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE SUITE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    (void)nya_filesystem_delete_recursive(TEST_SIGNATURE_ROOT);
    defer (void)nya_filesystem_delete_recursive(TEST_SIGNATURE_ROOT);

    NYA_Arena* arena = nya_arena_create(.name = "test_plugin_signature");
    defer      nya_arena_destroy(arena);

    u32 failures = 0;

    NYA_CryptoSignKeyPair publisher = { 0 };
    NYA_CryptoSignKeyPair impostor  = { 0 };
    nya_assert(nya_crypto_sign_key_pair_create(&publisher).ok);
    nya_assert(nya_crypto_sign_key_pair_create(&impostor).ok);
    defer nya_crypto_sign_key_pair_destroy(&publisher);
    defer nya_crypto_sign_key_pair_destroy(&impostor);

    plugin_write("demo");
    NYA_ConstCString demo = plugin_directory(arena, "demo");

    printf("TEST: the digest is one stable value over the plugin's code\n");
    {
        NYA_CryptoSha256Digest first  = { 0 };
        NYA_CryptoSha256Digest second = { 0 };
        nya_assert(nya_plugin_digest(demo, &first).ok, "a well formed plugin has a digest");
        nya_assert(nya_plugin_digest(demo, &second).ok);
        nya_assert(nya_memcmp(first.bytes, second.bytes, sizeof(first.bytes)) == 0, "the digest is not deterministic");

        // A directory with no manifest or no entry point is not a plugin, and has no digest.
        NYA_CryptoSha256Digest none = { 0 };
        nya_assert(!nya_plugin_digest(TEST_SIGNATURE_ROOT, &none).ok, "a directory that is not a plugin returned a digest");

        printf("  a plugin hashes the same twice, and a non-plugin hashes to nothing\n");
    }

    printf("TEST: a signature by a pinned key verifies, using the crypto API directly\n");
    {
        // The primitive, on the plugin's own digest: this is what nya_plugin_signature_write signs and
        // nya_plugin_signature_verify checks, shown here against the raw Ed25519 calls with a test key.
        NYA_CryptoSha256Digest digest = { 0 };
        nya_assert(nya_plugin_digest(demo, &digest).ok);

        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&publisher.secret_key, digest.bytes, sizeof(digest.bytes), &signature);

        nya_assert(nya_crypto_sign_verify(&publisher.public_key, digest.bytes, sizeof(digest.bytes), &signature),
                   "the publisher's own signature over the digest did not verify");
        nya_assert(!nya_crypto_sign_verify(&impostor.public_key, digest.bytes, sizeof(digest.bytes), &signature),
                   "the digest verified under a key that did not sign it");

        NYA_CryptoSha256Digest altered = digest;
        altered.bytes[0]              ^= 0x01;
        nya_assert(!nya_crypto_sign_verify(&publisher.public_key, altered.bytes, sizeof(altered.bytes), &signature),
                   "an altered digest still verified");

        printf("  the right key over the right digest verifies; a wrong key and a changed digest do not\n");
    }

    printf("TEST: a plugin signed by a pinned publisher verifies\n");
    {
        _nya_plugin_trusted_keys_reset();
        nya_assert(nya_plugin_trust_key("test-publisher", &publisher.public_key).ok);

        nya_assert(nya_plugin_signature_write(demo, "test-publisher", &publisher.secret_key, &publisher.public_key).ok,
                   "signing the plugin failed");

        char who[NYA_PLUGIN_PUBLISHER_MAX] = { 0 };
        NYA_Error verified                 = nya_plugin_signature_verify(demo, who, sizeof(who));
        nya_assert(verified.ok, "a correctly signed plugin was not accepted");
        nya_assert(nya_string_equals(who, "test-publisher"), "the wrong publisher was reported: '%s'", who);

        printf("  it verifies against the pinned key and reports the pinned publisher\n");
    }

    printf("TEST: any change to a signed plugin is refused\n");
    {
        // The signature from the phase above still on disk; changing a byte of the code must break it.
        plugin_file_write("demo", NYA_PLUGIN_ENTRY_FILE, "function on_load() end -- tampered\n");

        NYA_Error verified = nya_plugin_signature_verify(demo, nullptr, 0);
        nya_assert(!verified.ok, "a tampered plugin still verified");
        nya_assert(verified.kind == NYA_ERROR_PERMISSION_DENIED, "a tampered plugin was refused for the wrong reason");

        // Put it back, and it verifies again: the refusal was the change, not the file write.
        plugin_file_write("demo", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");
        nya_assert(nya_plugin_signature_verify(demo, nullptr, 0).ok, "the restored plugin did not verify again");

        printf("  a flipped byte breaks the signature, and restoring it heals it\n");
    }

    printf("TEST: a signature by an unpinned key is refused\n");
    {
        // Signed by the impostor, whose key the program never pinned.
        nya_assert(nya_plugin_signature_write(demo, "impostor", &impostor.secret_key, &impostor.public_key).ok);

        NYA_Error verified = nya_plugin_signature_verify(demo, nullptr, 0);
        nya_assert(!verified.ok, "a plugin signed by an unpinned key was accepted");
        nya_assert(verified.kind == NYA_ERROR_PERMISSION_DENIED, "an unpinned signer was refused for the wrong reason");

        // Re-sign with the pinned publisher, and it verifies: trust is by key, not by the name in the file.
        nya_assert(nya_plugin_signature_write(demo, "test-publisher", &publisher.secret_key, &publisher.public_key).ok);
        nya_assert(nya_plugin_signature_verify(demo, nullptr, 0).ok);

        printf("  an unpinned signer is refused; the pinned one is not\n");
    }

    printf("TEST: an unsigned plugin has no signature to verify\n");
    {
        plugin_write("bare");
        NYA_ConstCString bare = plugin_directory(arena, "bare");

        NYA_Error verified = nya_plugin_signature_verify(bare, nullptr, 0);
        nya_assert(!verified.ok, "a plugin with no signature verified");
        nya_assert(verified.kind == NYA_ERROR_NOT_FOUND, "a missing signature was not reported as absent");

        printf("  no plugin.sig is NYA_ERROR_NOT_FOUND, not a match\n");
    }

    printf("TEST: the loader refuses what does not verify and admits what does\n");
    {
        _nya_plugin_reset_for_test();
        _nya_plugin_trusted_keys_reset();
        nya_assert(nya_plugin_trust_key("test-publisher", &publisher.public_key).ok);

        // The unsigned "bare" plugin: refused before anything of it runs, because this build requires a
        // signature. The refusal is a permission denial, not a Lua or manifest error.
        NYA_ConstCString bare   = plugin_directory(arena, "bare");
        NYA_Error        loaded = nya_plugin_load(bare);
        nya_assert(!loaded.ok, "an unsigned plugin was loaded by a build that requires signatures");
        nya_assert(loaded.kind == NYA_ERROR_PERMISSION_DENIED, "an unsigned plugin was refused for the wrong reason");
        nya_assert(nya_plugin_find("bare") == nullptr, "a refused plugin left a slot behind");

        // The signed "demo" gets past the signature gate. Whether the VM then runs depends on whether
        // this build has Lua; either way the loader does not refuse it for its signature.
        NYA_Error signed_load = nya_plugin_load(demo);
        nya_assert(signed_load.ok || signed_load.kind != NYA_ERROR_PERMISSION_DENIED,
                   "a correctly signed plugin was refused at the signature gate");

        nya_plugin_unload_all();

        printf("  unsigned is refused with a permission denial; a pinned signature passes the gate\n");
    }

    printf("%s: test_plugin_signature (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
