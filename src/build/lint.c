#include "build/build.h"

/* PRIVATE API DECLARATION */

/** One source file, lexed once and shared by every rule. */
typedef struct {
    NYA_String* path;
    NYA_String* text;
    NYA_Lexer   lexer;

    /** Written by a preprocessor pass. Counted as a caller, never judged. */
    b8 generated;
} LintFile;
nya_derive_array(LintFile);

nya_derive_dict(u32);

/** A call the tree does not make outside platform/, and what to call instead. */
typedef struct {
    NYA_ConstCString name;
    NYA_ConstCString instead;
} _LintBannedCall;

/** A module's place in the target order. A module may include only modules of a lower rank. */
typedef struct {
    NYA_ConstCString name;
    u32              rank;
} _LintModule;

/** An include edge that breaks the order today, and how many includes of it there are. The count only falls. */
typedef struct {
    NYA_ConstCString from;
    NYA_ConstCString to;
    u32              count;
} _LintEdge;

/** A name a rule lets through, and why. Reviewed by hand; an entry that stops being needed is a finding. */
typedef struct {
    NYA_ConstCString name;
    NYA_ConstCString reason;
} _LintAllowed;

/** Everything a rule reads and the one number it writes. */
typedef struct {
    NYA_ArrayᐸLintFileᐳ* files;
    u32                  findings;
} Lint;

/** The directories and files the rules read. Vendors are not ours to judge; corpora are not source. */
NYA_INTERNAL const NYA_ConstCString _LINT_ROOTS[] = {
    "./src/nyangine-std", "./src/nyangine-core", "./src/nyangine-ui", "./src/nyangine-plugins", "./src/gnyame", "./src/build", "./src/genyarated", "./examples", "./tests", "./bench",
};
NYA_INTERNAL const NYA_ConstCString _LINT_FILES[] = { "./src/main.c", "./build.c" };

/* CONSTANTS */

NYA_INTERNAL const _LintBannedCall _LINT_BANNED_CALLS[] = {
    { "malloc",   "arenas own memory here; take an NYA_Arena" },
    { "calloc",   "arenas own memory here; take an NYA_Arena" },
    { "realloc",  "arenas own memory here; take an NYA_Arena" },
    { "free",     "arenas own memory here; destroy the arena" },
    { "alloca",   "an unbounded stack allocation; use a fixed buffer with its bound asserted" },
    { "strcpy",   "no bound on the write; use snprintf or nya_string_*" },
    { "strcat",   "no bound on the write; use snprintf or nya_string_*" },
    { "sprintf",  "no bound on the write; use snprintf" },
    { "vsprintf", "no bound on the write; use vsnprintf" },
    { "gets",     "no bound on the write, removed from C11" },
    { "strtok",   "hidden global state; walk the string with the lexer or nya_string_*" },
    { "atoi",     "cannot report a bad number; parse with strtol and check the end pointer" },
    { "atol",     "cannot report a bad number; parse with strtol and check the end pointer" },
    { "atof",     "cannot report a bad number; parse with strtod and check the end pointer" },
    { "rand",     "one hidden global stream, not reproducible per seed; use nya_rng or hash a counter" },
    { "srand",    "one hidden global stream, not reproducible per seed; use nya_rng or hash a counter" },
    { "system",   "runs a shell over its argument; use nya_command with an argument vector" },
};

/*
 * The order from TODO.md's "Target architecture", in today's module names. Equal ranks sit beside each other and
 * include neither. `nn` is a pure library above `math`, `net` the transport below the app loop, and `core` is what
 * the roadmap calls `app`.
 */
NYA_INTERNAL const _LintModule _LINT_MODULES[] = {
    { "os",       0 }, { "base",     1 }, { "platform", 2 }, { "math",    2 }, { "serde",   3 }, { "nn",      3 }, { "crypto",  3 },
    { "permission", 3 },
    // template renders a NYA_Object to text and includes only base, so it sits beside serde: a sibling that turns an object into a document rather than a wire format, and neither includes the other.
    { "template", 3 },
    // console is a command registry over base alone, dispatched through by a dev console and a command palette; it sits low beside template.
    { "console",  3 },
    // db is above crypto and base and below everything that stores anything, which is why it shares net's rank rather than sitting under it: neither includes the other and neither ever should.
    { "db",       4 },
    { "net",      4 },
    // tls is beside them: a socket with a library on it, above os and base and below the http server that is the only thing here with a reason to want one.
    { "tls",      4 },
    // accounts is above db, crypto and permission and below http, because a program with no HTTP server at all still has users: a CLI making the first account, a game with a control socket.
    { "accounts", 5 },
    // smtp is a mail client above tls, whose client session it borrows, and below http, the one thing here with a reason to send a verification mail. It shares accounts' rank: neither includes the other.
    { "smtp",     5 },
    // acme is beside smtp: a certificate client that signs with crypto and reaches the CA over a transport the program wires, so it needs neither the socket nor http. It holds the HTTP-01 challenge as data an http route reads, which is why http depends on nothing of it and it stays here.
    { "acme",     5 },
    { "http",     6 }, { "core",     7 },
    // replicate is net's other half: a world on the wire rather than bytes on it, so it is written in entities and sits above the app loop where net sits below it. See replicate.h.
    { "replicate", 8 }, { "renderer", 8 }, { "ui",      9 }, { "physics", 10 }, { "debug",   11 },
    { "testing",  12 }, { "plugins", 12 },
};

/**
 * Headers every module may include whatever its rank: type names, attribute macros and the libc
 * includes. They declare no function, so nothing can depend on them in the sense the rule is about.
 * */
NYA_INTERNAL const NYA_ConstCString _LINT_PRELUDE[] = {
    "base/base_basic.h",
    "base/base_types.h",
    "base/base_attributes.h",
};

/**
 * Words that make a field a secret by its name alone.
 *
 * The same four src/nyangine/http/http_log.c redacts a query parameter for, and for the same reason: a
 * name is the only evidence there is where nothing describes the value. A reflected field named like
 * one of these carries `@redact` (or `@secret`, which redacts a log too and encrypts the field on
 * disk besides), or says `@loggable` to mean it on purpose.
 * */
NYA_INTERNAL const NYA_ConstCString _LINT_SECRET_WORDS[] = { "password", "token", "secret", "code" };

/** The pairs from the style guide's verb vocabulary, with the engine's own init/deinit for init/shutdown. */
NYA_INTERNAL const NYA_ConstCString _LINT_VERB_PAIRS[][2] = {
    { "create", "destroy" }, { "init", "deinit" }, { "start", "stop" },     { "begin", "end" },           { "open", "close" },
    { "acquire", "release" }, { "lock", "unlock" }, { "push", "pop" },     { "add", "remove" },          { "attach", "detach" },
    { "enable", "disable" }, { "bind", "unbind" },  { "subscribe", "unsubscribe" },
};

/** Where a rule reports, in the form an editor and a terminal both make clickable. */
NYA_INTERNAL void _lint_report(Lint* lint, NYA_ConstCString rule, const NYA_String* path, u32 line, NYA_ConstCString format, ...)
    __attr_fmt_printf(5, 6);

NYA_INTERNAL b8 _lint_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL void _lint_file_add(Lint* lint, NYA_ConstCString path);

/** The token's text equals `text`. */
NYA_INTERNAL b8 _lint_token_is(const LintFile* file, const NYA_Token* token, NYA_ConstCString text) __attr_no_discard;

/** The token is the symbol `symbol`. */
NYA_INTERNAL b8 _lint_symbol_is(const NYA_Token* token, u8 symbol) __attr_no_discard;

/** Copies the token's text into `arena` as a string. */
NYA_INTERNAL NYA_String* _lint_token_text(NYA_Arena* arena, const LintFile* file, const NYA_Token* token) __attr_no_discard;

/**
 * Every name a header declares NYA_API, in order, once per declaration. Only the form the tree uses:
 * `NYA_API <type> name(`.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _lint_api_names(const LintFile* file) __attr_no_discard;

/** The module's place in the order, or null when it has none. */
NYA_INTERNAL const _LintModule* _lint_module_find(const NYA_String* name) __attr_no_discard;

/** The module a path under src/nyangine/ belongs to, or null for anything else. */
NYA_INTERNAL NYA_String* _lint_module_of(NYA_Arena* arena, const NYA_String* path) __attr_no_discard;

/**
 * Whether the comment token at `index` carries `marker` at the start of one of its lines, which is
 * exactly where src/build/pp/reflection.c looks for an annotation. Written to agree with it: a rule
 * that accepted a spelling the generator ignores would pass a field that is never redacted.
 * */
NYA_INTERNAL b8 _lint_comment_has(const LintFile* file, u64 index, NYA_ConstCString marker) __attr_no_discard;

/** Whether `name` holds `word`, neither of them case sensitive. */
NYA_INTERNAL b8 _lint_name_holds(const NYA_String* name, NYA_ConstCString word) __attr_no_discard;

NYA_INTERNAL void _lint_rule_lexed(Lint* lint);
NYA_INTERNAL void _lint_rule_banned_calls(Lint* lint);
NYA_INTERNAL void _lint_rule_layering(Lint* lint);
NYA_INTERNAL void _lint_rule_verb_pairs(Lint* lint);
NYA_INTERNAL void _lint_rule_callers(Lint* lint);
NYA_INTERNAL void _lint_rule_clangd(Lint* lint);
NYA_INTERNAL void _lint_rule_redact(Lint* lint);
NYA_INTERNAL void _lint_rule_web_profile(Lint* lint);

// The allowances: what each rule knowingly lets through today, and why. After the declarations it reads.
#include "build/lint_allowances.h"

/* PUBLIC API IMPLEMENTATION */

u32 lint_run(void) {
    u64 started = nya_clock_get_monotonic_ns();

    Lint lint = { .files = nya_array_create(nya_arena_global, LintFile) };

    for (u32 i = 0; i < nya_carray_length(_LINT_ROOTS); i++) {
        NYA_EXPECT(nya_filesystem_walk(nya_arena_global, _LINT_ROOTS[i], _lint_collect, &lint));
    }
    for (u32 i = 0; i < nya_carray_length(_LINT_FILES); i++) _lint_file_add(&lint, _LINT_FILES[i]);

    _lint_rule_lexed(&lint);
    _lint_rule_banned_calls(&lint);
    _lint_rule_layering(&lint);
    _lint_rule_verb_pairs(&lint);
    _lint_rule_callers(&lint);
    _lint_rule_clangd(&lint);
    _lint_rule_redact(&lint);
    _lint_rule_web_profile(&lint);

    nya_array_foreach (lint.files, file) nya_lexer_destroy(&file->lexer);

    u64 elapsed_ms = (nya_clock_get_monotonic_ns() - started) / 1000000;
    nya_log_info("Linted " FMTu64 " files in " FMTu64 " ms: %u finding%s.", lint.files->length, elapsed_ms, lint.findings, lint.findings == 1 ? "" : "s");

    return lint.findings;
}

/* RULES */

/*
 * Every other rule trusts the tokens, so a file the lexer misread is a finding of its own rather than a silent gap:
 * C allows no raw newline inside a string or a character literal, and one there means a quote was misread and the
 * rest of the file was swallowed. That is how the C23 digit separator in 0x8000'0000U first hid a whole file.
 */
void _lint_rule_lexed(Lint* lint) {
    nya_array_foreach (lint->files, file) {
        nya_array_foreach (file->lexer.tokens, token) {
            if (token->type != NYA_TOKEN_STRING && token->type != NYA_TOKEN_CHARACTER) continue;

            NYA_ConstCString text = &file->lexer.source[token->source_location];
            for (u32 i = 0; i < token->length; i++) {
                if (text[i] != '\n' || (i > 0 && text[i - 1] == '\\')) continue;

                _lint_report(lint, "lexed", file->path, token->line_number, "a literal runs over a line end, so this file was misread from here on");
                break;
            }
        }
    }
}

/*
 * Calls that bypass what the engine provides in their place. platform/ is where the engine meets the OS and may
 * use them; everything else goes through it. tests/cbmc/ is the other exception: those harnesses are fed to CBMC,
 * not linked into the engine, and CBMC's object model needs a real malloc to give a buffer an exact, nondet size
 * so its bounds check can catch a read or write one byte past the end — the whole point of the proof.
 */
void _lint_rule_banned_calls(Lint* lint) {
    nya_array_foreach (lint->files, file) {
        if (file->generated || nya_string_contains(file->path, "src/nyangine-std/platform/") || nya_string_contains(file->path, "tests/cbmc/")) continue;

        NYA_ArrayᐸNYA_Tokenᐳ* tokens = file->lexer.tokens;
        for (u64 i = 0; i + 1 < tokens->length; i++) {
            NYA_Token* token = &tokens->items[i];
            if (token->type != NYA_TOKEN_IDENT || !_lint_symbol_is(&tokens->items[i + 1], '(')) continue;

            // a member called like the function, `table.free(x)` or `vtable->free(x)`, is not the function.
            if (i > 0 && (_lint_symbol_is(&tokens->items[i - 1], '.') || _lint_symbol_is(&tokens->items[i - 1], '>'))) continue;

            for (u32 banned = 0; banned < nya_carray_length(_LINT_BANNED_CALLS); banned++) {
                if (!_lint_token_is(file, token, _LINT_BANNED_CALLS[banned].name)) continue;

                _lint_report(lint, "banned-call", file->path, token->line_number, "%s: %s", _LINT_BANNED_CALLS[banned].name, _LINT_BANNED_CALLS[banned].instead);
            }
        }
    }
}

/*
 * A module includes only modules below it in the target order from TODO.md's roadmap. Where the tree does not yet,
 * the edge is allowed with its count, and the count may only fall.
 */
void _lint_rule_layering(Lint* lint) {
    u32 counts[nya_carray_length(_LINT_LAYERING_ALLOWED)] = {};

    nya_array_foreach (lint->files, file) {
        NYA_String* from = _lint_module_of(nya_arena_global, file->path);
        if (from == nullptr) continue;

        const _LintModule* from_module = _lint_module_find(from);
        if (from_module == nullptr) {
            _lint_report(lint, "layering", file->path, 1, "module '%.*s' has no place in the order; add it to _LINT_MODULES", (int)from->length, from->items);
            continue;
        }

        NYA_ArrayᐸNYA_Tokenᐳ* tokens = file->lexer.tokens;
        for (u64 i = 0; i + 2 < tokens->length; i++) {
            if (!_lint_symbol_is(&tokens->items[i], '#') || !_lint_token_is(file, &tokens->items[i + 1], "include")) continue;

            NYA_Token* target = &tokens->items[i + 2];
            if (target->type != NYA_TOKEN_STRING) continue;

            NYA_String* included = _lint_token_text(nya_arena_global, file, target);
            NYA_String* to       = _lint_module_of(nya_arena_global, nya_string_sprintf(nya_arena_global, "src/%.*s", (int)included->length, included->items));

            // the umbrella header and same-module includes say nothing about the order.
            if (to == nullptr || nya_string_equals(to, from)) continue;

            // the prelude is below every module, including the lowest: it is types, attributes and the libc includes, with no code in it at all, so depending on it cannot invert anything.
            b8 prelude = false;
            for (u32 index = 0; index < nya_carray_length(_LINT_PRELUDE); index++) {
                prelude |= nya_string_ends_with(included, _LINT_PRELUDE[index]);
            }
            if (prelude) continue;

            const _LintModule* to_module = _lint_module_find(to);
            if (to_module == nullptr) continue;
            if (to_module->rank < from_module->rank) continue;

            b8 allowed = false;
            for (u32 edge = 0; edge < nya_carray_length(_LINT_LAYERING_ALLOWED); edge++) {
                if (!nya_string_equals(from, _LINT_LAYERING_ALLOWED[edge].from) || !nya_string_equals(to, _LINT_LAYERING_ALLOWED[edge].to)) continue;

                counts[edge]++;
                allowed = counts[edge] <= _LINT_LAYERING_ALLOWED[edge].count;
            }
            if (allowed) continue;

            _lint_report(lint, "layering", file->path, target->line_number, "%.*s includes %.*s, which sits at or above it in the module order",
                         (int)from->length, from->items, (int)to->length, to->items);
        }
    }

    for (u32 edge = 0; edge < nya_carray_length(_LINT_LAYERING_ALLOWED); edge++) {
        if (counts[edge] >= _LINT_LAYERING_ALLOWED[edge].count) continue;

        _lint_report(lint, "layering", nya_string_from(nya_arena_global, "./src/build/lint_allowances.h"), 1,
                     "%s -> %s is allowed %u includes and has %u: lower the allowance so it cannot grow back", _LINT_LAYERING_ALLOWED[edge].from,
                     _LINT_LAYERING_ALLOWED[edge].to, _LINT_LAYERING_ALLOWED[edge].count, counts[edge]);
    }
}

/*
 * Every verb ships with its partner in the same header. A public macro counts as a declaration, since several
 * constructors are macros over a function taking an options struct.
 */
void _lint_rule_verb_pairs(Lint* lint) {
    b8 used[nya_carray_length(_LINT_VERB_PAIRS_ALLOWED)] = {};

    nya_array_foreach (lint->files, file) {
        if (!nya_string_ends_with(file->path, ".h") || !nya_string_contains(file->path, "src/nyangine-")) continue;

        NYA_ArrayᐸNYA_Stringᐳ* names = _lint_api_names(file);

        // public macros: `#define nya_thing_create(`.
        NYA_ArrayᐸNYA_Tokenᐳ* tokens = file->lexer.tokens;
        for (u64 i = 0; i + 3 < tokens->length; i++) {
            if (!_lint_symbol_is(&tokens->items[i], '#') || !_lint_token_is(file, &tokens->items[i + 1], "define")) continue;

            NYA_Token* name = &tokens->items[i + 2];
            if (name->type != NYA_TOKEN_IDENT || !_lint_symbol_is(&tokens->items[i + 3], '(')) continue;

            NYA_String* text = _lint_token_text(nya_arena_global, file, name);
            if (nya_string_starts_with(text, "nya_")) nya_array_push_back(names, *text);
        }

        for (u64 index = 0; index < names->length; index++) {
            NYA_String* name = &names->items[index];

            // overloads share a name and are judged once; a predicate like _is_open is a question, not a verb.
            b8 repeated = false;
            for (u64 earlier = 0; earlier < index; earlier++) {
                if (nya_string_equals(&names->items[earlier], name)) repeated = true;
            }
            if (repeated || nya_string_contains(name, "_is_")) continue;

            // a private name exists for a public macro, which is what gets paired.
            if (nya_string_starts_with(name, "_")) continue;

            for (u32 pair = 0; pair < nya_carray_length(_LINT_VERB_PAIRS); pair++) {
                for (u32 side = 0; side < 2; side++) {
                    NYA_ConstCString verb    = _LINT_VERB_PAIRS[pair][side];
                    NYA_ConstCString partner = _LINT_VERB_PAIRS[pair][1 - side];

                    NYA_String* suffix = nya_string_sprintf(nya_arena_global, "_%s", verb);
                    if (!nya_string_ends_with(name, nya_string_to_cstring(nya_arena_global, suffix))) continue;

                    NYA_String* wanted = nya_string_sprintf(nya_arena_global, "%.*s%s", (int)(name->length - strlen(verb)), name->items, partner);

                    b8 found = false;
                    nya_array_foreach (names, other) {
                        if (nya_string_equals(other, wanted)) found = true;
                    }
                    if (found) continue;

                    b8 allowed = false;
                    for (u32 entry = 0; entry < nya_carray_length(_LINT_VERB_PAIRS_ALLOWED); entry++) {
                        if (!nya_string_equals(name, _LINT_VERB_PAIRS_ALLOWED[entry].name)) continue;
                        allowed     = true;
                        used[entry] = true;
                    }
                    if (allowed) continue;

                    _lint_report(lint, "verb-pair", file->path, 1, "%.*s has no %.*s beside it", (int)name->length, name->items, (int)wanted->length,
                                 wanted->items);
                }
            }
        }
    }

    for (u32 entry = 0; entry < nya_carray_length(_LINT_VERB_PAIRS_ALLOWED); entry++) {
        if (used[entry]) continue;

        _lint_report(lint, "verb-pair", nya_string_from(nya_arena_global, "./src/build/lint_allowances.h"), 1,
                     "%s is allowed without a partner and no longer needs to be: remove the entry", _LINT_VERB_PAIRS_ALLOWED[entry].name);
    }
}

/*
 * Every NYA_API has a caller: its name appears somewhere beyond its own declaration and definition. Counted over
 * identifiers in every file, generated ones included, so a call made only through a macro counts because the
 * macro's body names it. Overloads are declared and defined once per signature, so they count their own pairs.
 */
void _lint_rule_callers(Lint* lint) {
    NYA_Dictᐸu32ᐳ* seen = nya_dict_create(nya_arena_global, u32);

    nya_array_foreach (lint->files, file) {
        nya_array_foreach (file->lexer.tokens, token) {
            if (token->type != NYA_TOKEN_IDENT || token->length < 4) continue;

            // only the names the rule asks about, which keeps the table to the engine's own vocabulary.
            NYA_ConstCString start = &file->lexer.source[token->source_location];
            if (nya_memcmp(start, "nya_", 4) != 0 && (token->length < 5 || nya_memcmp(start, "_nya_", 5) != 0)) continue;

            NYA_CString name  = nya_string_to_cstring(nya_arena_global, _lint_token_text(nya_arena_global, file, token));
            u32*        count = nya_dict_get(seen, name);
            if (count != nullptr) {
                *count += 1;
            } else {
                nya_dict_add(seen, name, 1U);
            }
        }
    }

    b8 used[nya_carray_length(_LINT_CALLERS_ALLOWED)] = {};

    nya_array_foreach (lint->files, file) {
        if (!nya_string_ends_with(file->path, ".h") || !nya_string_contains(file->path, "src/nyangine-")) continue;

        NYA_ArrayᐸNYA_Stringᐳ* names = _lint_api_names(file);
        nya_array_foreach (names, name) {
            NYA_CString name_text  = nya_string_to_cstring(nya_arena_global, name);
            u32*        count      = nya_dict_get(seen, name_text);
            u32         appearances = count != nullptr ? *count : 0;

            // declared once per overload in this header, and defined as often in some .c.
            u32 declarations = 0;
            nya_array_foreach (names, other) {
                if (nya_string_equals(other, name)) declarations++;
            }
            if (appearances > declarations * 2) continue;

            b8 allowed = false;
            for (u32 entry = 0; entry < nya_carray_length(_LINT_CALLERS_ALLOWED); entry++) {
                if (!nya_string_equals(name, _LINT_CALLERS_ALLOWED[entry].name)) continue;
                allowed     = true;
                used[entry] = true;
            }
            if (allowed) continue;

            _lint_report(lint, "caller", file->path, 1, "%s is declared and defined and called by nothing: give it a caller, a test, or delete it", name_text);
        }
    }

    for (u32 entry = 0; entry < nya_carray_length(_LINT_CALLERS_ALLOWED); entry++) {
        if (used[entry]) continue;

        _lint_report(lint, "caller", nya_string_from(nya_arena_global, "./src/build/lint_allowances.h"), 1,
                     "%s is allowed to have no caller and now has one, or is gone: remove the entry", _LINT_CALLERS_ALLOWED[entry].name);
    }
}

/*
 * .clangd carries its own copy of the compile flags, and three times a flag added to the build and not to it left a
 * whole module analysed as an empty translation unit. Every define and include the project compiles with must
 * appear there. The component system generates .clangd and retires this rule.
 */
void _lint_rule_clangd(Lint* lint) {
    NYA_String* clangd = nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read("./.clangd", clangd));

    NYA_String* clangd_path = nya_string_from(nya_arena_global, "./.clangd");

    NYA_ArrayᐸNYA_Stringᐳ* expected = nya_array_create(nya_arena_global, NYA_String);

    NYA_ConstCString plugins[] = { FLAGS_PLUGINS };
    for (u32 i = 0; i < nya_carray_length(plugins); i++) nya_array_push_back(expected, *nya_string_from(nya_arena_global, plugins[i]));

    // the host's vendors, since those are the headers that exist on the machine the editor runs on.
    NYA_VendorRule* vendors[] = {
#if OS_WINDOWS
        NYA_PROJECT_VENDORS_WINDOWS_X86_64,
#else
        NYA_PROJECT_VENDORS_LINUX_X86_64,
#endif
    };
    for (u32 i = 0; i < nya_carray_length(vendors); i++) {
        for (u32 flag = 0; flag < NYA_VENDOR_MAX_FLAGS && vendors[i]->cflags[flag] != nullptr; flag++) {
            if (nya_string_starts_with(vendors[i]->cflags[flag], "-D")) nya_array_push_back(expected, *nya_string_from(nya_arena_global, vendors[i]->cflags[flag]));
        }
        for (u32 flag = 0; flag < NYA_VENDOR_MAX_FLAGS && vendors[i]->includes[flag] != nullptr; flag++) {
            nya_array_push_back(expected, *nya_string_from(nya_arena_global, vendors[i]->includes[flag]));
        }
    }

    nya_array_foreach (expected, flag) {
        NYA_String* needle = nullptr;

        if (nya_string_starts_with(flag, "-I")) {
            // .clangd spells include paths absolute, so match the path from the repository root down.
            NYA_String* path = nya_string_from(nya_arena_global, nya_string_to_cstring(nya_arena_global, flag) + 2);
            nya_string_strip_prefix(path, "./");
            nya_string_strip_suffix(path, "/");
            needle = nya_string_sprintf(nya_arena_global, "/%.*s", (int)path->length, path->items);
        } else {
            needle = flag;
        }

        // the flag must end where a flag ends, so -DNYA_PLUGIN_LUA is not found inside -DNYA_PLUGIN_LUAJIT.
        b8          found  = false;
        NYA_CString text   = nya_string_to_cstring(nya_arena_global, clangd);
        NYA_CString target = nya_string_to_cstring(nya_arena_global, needle);
        for (NYA_CString at = strstr(text, target); at != nullptr && !found; at = strstr(at + 1, target)) {
            char after = at[needle->length];
            found      = after == ',' || after == '/' || after == ']' || after == '\n' || after == ' ' || after == '\0';
        }
        if (found) continue;

        _lint_report(lint, "clangd", clangd_path, 1, "%.*s is in the build and not in .clangd, so the editor analyses under other flags than the compiler",
                     (int)flag->length, flag->items);
    }
}

/*
 * A reflected field named like a secret carries `@redact`, or says out loud that it is meant to be read.
 *
 * Reflection is what writes a struct out — into a log record, a debug dump, an IPC message — so a field
 * it describes is a field that can appear somewhere a person reads, and `password`, `token`, `secret`
 * and `code` are the names that then cost something. The tag is a one-word decision and its absence is
 * indistinguishable from nobody having thought about it, which is what this rule turns into a failure.
 *
 * `@loggable` is the other answer, for a field whose name says secret and whose content does not: an
 * expiry, a count of codes, a token *type*. It has to be written, because the point is the decision.
 *
 * Scoped to the headers under src/nyangine and src/gnyame, which is exactly what the reflection pass
 * reads; a struct anywhere else has no generated table and nothing to tag.
 */
void _lint_rule_redact(Lint* lint) {
    nya_array_foreach (lint->files, file) {
        if (file->generated || !nya_string_ends_with(file->path, ".h")) continue;
        if (!nya_string_contains(file->path, "src/nyangine-") && !nya_string_contains(file->path, "src/gnyame/")) continue;

        NYA_ArrayᐸNYA_Tokenᐳ* tokens = file->lexer.tokens;

        for (u64 index = 0; index < tokens->length; index++) {
            if (!_lint_comment_has(file, index, "@reflect")) continue;

            u64 cursor = index + 1;
            while (cursor < tokens->length && tokens->items[cursor].type == NYA_TOKEN_COMMENT) cursor++;

            if (_lint_token_is(file, &tokens->items[cursor], "typedef")) cursor++;

            // an enum has variants rather than fields, and a variant carries no value to redact.
            if (!_lint_token_is(file, &tokens->items[cursor], "struct") && !_lint_token_is(file, &tokens->items[cursor], "union")) continue;

            while (cursor < tokens->length && !_lint_symbol_is(&tokens->items[cursor], '{')) cursor++;

            u32 depth = 0;

            for (; cursor < tokens->length; cursor++) {
                NYA_Token* token = &tokens->items[cursor];

                if (_lint_symbol_is(token, '{')) depth++;
                if (_lint_symbol_is(token, '}') && --depth == 0) break;

                // a declarator, by the same shape the reflection pass reads: a name, then the end of the declaration, another declarator, or an array extent.
                if (token->type != NYA_TOKEN_IDENT || cursor + 1 >= tokens->length) continue;
                if (!_lint_symbol_is(&tokens->items[cursor + 1], ';') && !_lint_symbol_is(&tokens->items[cursor + 1], ',') &&
                    !_lint_symbol_is(&tokens->items[cursor + 1], '[')) {
                    continue;
                }

                NYA_String* name = _lint_token_text(nya_arena_global, file, token);

                NYA_ConstCString word = nullptr;
                for (u32 entry = 0; entry < nya_carray_length(_LINT_SECRET_WORDS) && word == nullptr; entry++) {
                    if (_lint_name_holds(name, _LINT_SECRET_WORDS[entry])) word = _LINT_SECRET_WORDS[entry];
                }
                if (word == nullptr) continue;

                // the annotation sits in a comment on the field's own line, which is where the
                // reflection pass reads `@key` and `@skip` from too.
                b8 answered = false;
                for (u64 look = cursor + 1; look < tokens->length && look < cursor + 6 && !answered; look++) {
                    if (tokens->items[look].type != NYA_TOKEN_COMMENT || tokens->items[look].line_number != token->line_number) continue;

                    answered = _lint_comment_has(file, look, "@redact") || _lint_comment_has(file, look, "@loggable") ||
                               _lint_comment_has(file, look, "@skip") || _lint_comment_has(file, look, "@secret");
                }
                if (answered) continue;

                _lint_report(lint, "redact", file->path, token->line_number,
                             "%.*s is a reflected field named like a '%s': tag it @redact, or @loggable to say it may be read",
                             (int)name->length, name->items, word);
            }
        }
    }
}

/*
 * The web-profile gate of "Model, SO, DTO": only DTO headers reach the web client, so the three shapes
 * keep to their headers by name.
 *
 * A `*_dto.h` is the shared shape the `web` profile compiles, so it may include no server-only header:
 * an include of a `*_model.h` or `*_so.h` from a DTO header is exactly the edge that would drag the
 * server's storage layout, and any secret in it, into wasm. It may not include base_web_profile.h
 * either, since that header refuses to compile under the profile and would make the DTO refuse itself.
 *
 * A `*_model.h` or `*_so.h` is server only, and carries base_web_profile.h so the compiler refuses it
 * under -DNYA_WEB_PROFILE. This rule requires that include, so the compile-time half of the gate cannot
 * be forgotten: a model or so header without the guard is a hole this turns into a finding. Together the
 * two halves mean a header that breaks the split fails `./build check` and, in a web build, fails to
 * compile. See src/nyangine/base/base_web_profile.h and TODO.md's "Model, SO, DTO".
 */
void _lint_rule_web_profile(Lint* lint) {
    NYA_ConstCString guard = "base/base_web_profile.h";

    nya_array_foreach (lint->files, file) {
        if (file->generated || !nya_string_ends_with(file->path, ".h")) continue;

        b8 is_dto   = nya_string_ends_with(file->path, "_dto.h");
        b8 is_model = nya_string_ends_with(file->path, "_model.h");
        b8 is_so    = nya_string_ends_with(file->path, "_so.h");
        if (!is_dto && !is_model && !is_so) continue;

        b8 guarded = false;

        NYA_ArrayᐸNYA_Tokenᐳ* tokens = file->lexer.tokens;
        for (u64 i = 0; i + 2 < tokens->length; i++) {
            if (!_lint_symbol_is(&tokens->items[i], '#') || !_lint_token_is(file, &tokens->items[i + 1], "include")) continue;

            NYA_Token* target = &tokens->items[i + 2];
            if (target->type != NYA_TOKEN_STRING) continue;

            NYA_String* included = _lint_token_text(nya_arena_global, file, target);

            if (nya_string_ends_with(included, guard)) {
                guarded = true;

                if (is_dto) {
                    _lint_report(lint, "web-profile", file->path, target->line_number,
                                 "a dto header includes the server-only guard %s; a *_dto.h is the shape the web profile compiles and must not refuse itself", guard);
                }
                continue;
            }

            if (!is_dto) continue;

            if (nya_string_ends_with(included, "_model.h") || nya_string_ends_with(included, "_so.h")) {
                _lint_report(lint, "web-profile", file->path, target->line_number,
                             "a dto header includes %.*s; only *_dto.h reaches the web client, so a DTO must not include a model or so header", (int)included->length,
                             included->items);
            }
        }

        if ((is_model || is_so) && !guarded) {
            _lint_report(lint, "web-profile", file->path, 1,
                         "a server-only header must include \"nyangine-std/base/base_web_profile.h\" first, so the web profile refuses to compile it");
        }
    }
}

/* PRIVATE API IMPLEMENTATION */

void _lint_report(Lint* lint, NYA_ConstCString rule, const NYA_String* path, u32 line, NYA_ConstCString format, ...) {
    nya_assert(lint != nullptr && rule != nullptr && path != nullptr && format != nullptr);

    char    message[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    NYA_ConstCString shown = nya_string_to_cstring(nya_arena_global, path);
    if (nya_string_starts_with(shown, "./")) shown += 2;

    (void)fprintf(stderr, "%s:%u: error: %s [%s]\n", shown, line, message, rule);
    lint->findings++;
}

b8 _lint_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    Lint* lint = (Lint*)user_data;

    // corpora and kept crashes are inputs to parsers, not source. Filtered per file, because returning false here ends the whole walk, and a directory's callback comes after its children anyway.
    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (nya_string_contains(path, "/corpus/") || nya_string_contains(path, "/crashes/")) return true;

    if (nya_string_ends_with(entry->name, ".c") || nya_string_ends_with(entry->name, ".h")) _lint_file_add(lint, path);

    return true;
}

void _lint_file_add(Lint* lint, NYA_ConstCString path) {
    LintFile file = {
        .path = nya_string_from(nya_arena_global, path),
        .text = nya_string_create(nya_arena_global),
    };
    NYA_EXPECT(nya_file_read(path, file.text));

    if (!nya_string_starts_with(file.path, "./")) nya_string_extend_front(file.path, "./");
    file.generated = nya_string_starts_with(file.path, "./src/genyarated/");

    // UTF-8 names, since a derived type like NYA_ArrayᐸNYA_Stringᐳ is one identifier; character literals, so the quote in '"' cannot open a string that swallows the rest of the file.
    file.lexer = nya_lexer_create(nya_string_to_cstring(nya_arena_global, file.text), NYA_LEXER_UTF8_IDENTS | NYA_LEXER_CHAR_LITERALS);
    nya_lexer_run(&file.lexer);

    nya_array_push_back(lint->files, file);
}

b8 _lint_token_is(const LintFile* file, const NYA_Token* token, NYA_ConstCString text) {
    u64 length = strlen(text);
    return token->length == length && nya_memcmp(&file->lexer.source[token->source_location], text, length) == 0;
}

b8 _lint_symbol_is(const NYA_Token* token, u8 symbol) {
    return token->type == NYA_TOKEN_SYMBOL && token->symbol == symbol;
}

NYA_String* _lint_token_text(NYA_Arena* arena, const LintFile* file, const NYA_Token* token) {
    return nya_string_sprintf(arena, "%.*s", (int)token->length, &file->lexer.source[token->source_location]);
}

b8 _lint_comment_has(const LintFile* file, u64 index, NYA_ConstCString marker) {
    if (index >= file->lexer.tokens->length) return false;

    NYA_Token token = file->lexer.tokens->items[index];
    if (token.type != NYA_TOKEN_COMMENT) return false;

    u64 size = strlen(marker);
    if (token.length < size) return false;

    NYA_ConstCString body = &file->lexer.source[token.source_location];

    for (u64 at = 0; at + size <= token.length; at++) {
        if (nya_memcmp(body + at, marker, size) != 0) continue;

        // only at the start of a line within the comment, so prose that names an annotation is prose.
        b8 at_line_start = true;

        for (u64 back = at; back > 0; back--) {
            u8 previous = (u8)body[back - 1];

            if (previous == '\n') break;
            if (previous == ' ' || previous == '\t' || previous == '*') continue;

            at_line_start = false;
            break;
        }

        if (at_line_start) return true;
    }

    return false;
}

b8 _lint_name_holds(const NYA_String* name, NYA_ConstCString word) {
    u64 size = strlen(word);
    if (name->length < size) return false;

    for (u64 at = 0; at + size <= name->length; at++) {
        b8 same = true;

        for (u64 i = 0; i < size && same; i++) {
            char character = name->items[at + i];

            same = (character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character) == word[i];
        }

        if (same) return true;
    }

    return false;
}

NYA_ArrayᐸNYA_Stringᐳ* _lint_api_names(const LintFile* file) {
    NYA_ArrayᐸNYA_Stringᐳ* names  = nya_array_create(nya_arena_global, NYA_String);
    NYA_ArrayᐸNYA_Tokenᐳ*  tokens = file->lexer.tokens;

    for (u64 i = 0; i < tokens->length; i++) {
        if (tokens->items[i].type != NYA_TOKEN_IDENT || !_lint_token_is(file, &tokens->items[i], "NYA_API")) continue;

        // `#define NYA_API ...` is where the marker is made, not a declaration.
        if (i > 0 && _lint_token_is(file, &tokens->items[i - 1], "define")) continue;

        // the name is the last identifier before the first '(' of the declaration; a ';' first means a variable.
        for (u64 j = i + 1; j + 1 < tokens->length; j++) {
            if (_lint_symbol_is(&tokens->items[j], ';') || _lint_symbol_is(&tokens->items[j], '{')) break;
            if (tokens->items[j].type == NYA_TOKEN_IDENT && _lint_symbol_is(&tokens->items[j + 1], '(')) {
                // attributes before the name, like __attribute__((...)), are skipped past rather than taken.
                NYA_String* name = _lint_token_text(nya_arena_global, file, &tokens->items[j]);
                if (!nya_string_starts_with(name, "nya_") && !nya_string_starts_with(name, "_nya_")) continue;

                nya_array_push_back(names, *name);
                break;
            }
        }
    }

    return names;
}

const _LintModule* _lint_module_find(const NYA_String* name) {
    for (u32 i = 0; i < nya_carray_length(_LINT_MODULES); i++) {
        if (nya_string_equals(name, _LINT_MODULES[i].name)) return &_LINT_MODULES[i];
    }

    return nullptr;
}

NYA_String* _lint_module_of(NYA_Arena* arena, const NYA_String* path) {
    NYA_ConstCString text = nya_string_to_cstring(arena, path);

    // The ui toolkit is one module in its own subproject: every file under it is module "ui".
    if (strstr(text, "src/nyangine-ui/") != nullptr) return nya_string_from(arena, "ui");

    // Plugins are their own subproject; every file under it — acme included, now that it is a plugin — is module "plugins".
    if (strstr(text, "src/nyangine-plugins/") != nullptr) return nya_string_from(arena, "plugins");

    // std and core keep the module as the first path segment under the subproject root.
    NYA_ConstCString marker     = strstr(text, "src/nyangine-std/");
    u64              marker_len = strlen("src/nyangine-std/");
    if (marker == nullptr) {
        marker     = strstr(text, "src/nyangine-core/");
        marker_len = strlen("src/nyangine-core/");
    }
    if (marker == nullptr) return nullptr;

    NYA_ConstCString start = marker + marker_len;
    NYA_ConstCString slash = strchr(start, '/');

    // the umbrella nyangine.h/.c sits at the subproject root, not in a module.
    if (slash == nullptr) return nullptr;

    return nya_string_sprintf(arena, "%.*s", (int)(slash - start), start);
}
