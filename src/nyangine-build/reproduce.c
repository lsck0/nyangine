/**
 * @file reproduce.c
 *
 * `./build reproduce`: proof that the release build is bit-for-bit reproducible — the same commit gives
 * the same bytes, which is what lets someone who did not build a signed release check it against one they
 * build themselves.
 *
 * It builds the Linux release binary twice and compares the SHA-256 of the two, which catches every
 * source of run-to-run drift: a build timestamp, a random build-id, an unsorted link input. Then it reads
 * the produced binary back and asserts the absolute build directory does not appear anywhere in it, which
 * is what proves -ffile-prefix-map took (see FLAGS_REPRODUCIBLE_* in flags.h): a binary that still carried
 * its compilation directory would hash the same twice on one machine and differently on the next, so the
 * hash compare alone cannot see that leak.
 *
 * Linux only, like hook_verify_hardening: the scan reads an absolute path out of the ELF, and the Windows
 * archive still has the open zip-mtime question dist.c records. It builds the real release rule, so it
 * rebuilds whatever is stale first — run it where regenerating shaders and assets is safe (a normal
 * checkout or CI), not against a checkout whose compiled assets are shared read-only.
 * */
#include "nyangine-build/build.h"

#if !OS_WINDOWS

/* PRIVATE API DECLARATION */

/** The lowercase hex SHA-256 of `path`, via sha256sum, the way dist.c hashes an archive. */
NYA_INTERNAL NYA_CString _reproduce_sha256(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/** The absolute working directory, or nullptr when it cannot be read. */
NYA_INTERNAL NYA_ConstCString _reproduce_working_directory(NYA_Arena* arena) __attr_no_discard;

/** Builds the Linux release binary once and returns the SHA-256 of the bytes it produced. */
NYA_INTERNAL NYA_CString _reproduce_build_once(NYA_Arena* arena) __attr_no_discard;

/* PUBLIC API IMPLEMENTATION */

void reproduce_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "reproduce_runner");
    defer nya_arena_destroy(arena);

    nya_log_info("Reproducibility check: building %s twice and comparing the bytes.", LINUX_X86_64_BINARY);

    NYA_CString first = _reproduce_build_once(arena);

    // The absolute-path scan runs on the first binary, before the second overwrites it: a build directory left in the debug info is a leak the hash compare cannot see, since it is stable within one machine and only differs across them.
    NYA_ConstCString working_directory = _reproduce_working_directory(arena);
    if (working_directory != nullptr) {
        NYA_String* bytes = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(LINUX_X86_64_BINARY, bytes), "while reading %s to scan for absolute paths", LINUX_X86_64_BINARY);

        nya_assert_always(!nya_string_contains(bytes, working_directory),
                          "%s contains the absolute build directory '%s': -ffile-prefix-map did not take, so the binary is not path-independent.",
                          LINUX_X86_64_BINARY, working_directory);

        nya_log_info("No absolute build path in %s: -ffile-prefix-map took.", LINUX_X86_64_BINARY);
    } else {
        nya_log_warn("Could not read the working directory; skipping the absolute-path scan.");
    }

    NYA_CString second = _reproduce_build_once(arena);

    if (!nya_string_equals(first, second)) {
        nya_log_panic("%s is not reproducible: two builds of the same tree hashed %s and %s.", LINUX_X86_64_BINARY, first, second);
    }

    nya_log_info("Reproducible: two builds of %s both hashed %s.", LINUX_X86_64_BINARY, first);
}

/* PRIVATE API IMPLEMENTATION */

NYA_CString _reproduce_build_once(NYA_Arena* arena) {
    NYA_EXPECT(nya_build(&build_project_linux_x86_64), "while building %s for the reproducibility check", LINUX_X86_64_BINARY);
    return _reproduce_sha256(arena, LINUX_X86_64_BINARY);
}

NYA_CString _reproduce_sha256(NYA_Arena* arena, NYA_ConstCString path) {
    NYA_String* output = build_capture(arena, "sha256sum", (const NYA_ConstCString[]){ path, nullptr });

    // "<64 hex>  <path>", so the digest is the first word.
    NYA_ArrayᐸNYA_Stringᐳ* words = nya_string_split_words(arena, output);
    nya_assert(words->length > 0, "sha256sum printed nothing for '%s'.", path);

    NYA_CString digest = nya_string_to_cstring(arena, &words->items[0]);
    nya_assert(strlen(digest) == 64, "sha256sum printed a %zu character digest for '%s'.", strlen(digest), path);

    return digest;
}

NYA_ConstCString _reproduce_working_directory(NYA_Arena* arena) {
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) return nullptr;

    return nya_string_to_cstring(arena, nya_string_from(arena, buffer));
}

#else

void reproduce_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    nya_log_warn("`./build reproduce` checks the Linux release binary and runs only on a Linux host for now; the Windows archive's zip-mtime question is open, see dist.c.");
}

#endif
