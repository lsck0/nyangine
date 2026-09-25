#include "nyangine-core/nyangine.h"

#include "nyangine-build/build.h"

/* PRIVATE API DECLARATION */

/**
 * One module of the engine, in the order the cheatsheet lists them: what a reader needs first
 * comes first, rather than what the filesystem happens to sort first.
 * */
typedef struct {
    NYA_ConstCString directory;
    NYA_ConstCString title;
    NYA_ConstCString blurb;
} _NYA_CheatModule;

/** A cursor over a header's bytes, handing out one trimmed line at a time. */
typedef struct {
    const char* text;
    u64         length;
    u64         offset;
} _NYA_CheatReader;

/**
 * The three lists one header contributes. They are filled in source order and written out grouped,
 * because a reader looking for a struct should not have to scan past forty functions to find it.
 * */
typedef struct {
    NYA_String* types;
    NYA_String* macros;
    NYA_String* functions;

    /** Macro names already listed for this header. The two arms of an #if define the same names. */
    char seen[NYA_CHEATSHEET_MAX_SEEN][128];
    u32  seen_count;

    /** First sentence of the doc comment directly above the declaration being read, or empty. */
    char summary[NYA_CHEATSHEET_MAX_SUMMARY + 1];
} _NYA_CheatFile;

NYA_INTERNAL b8 _nya_cheatsheet_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _nya_cheatsheet_compare(const NYA_String* a, const NYA_String* b);

/** Next line, trimmed of surrounding whitespace. False at the end of the text. */
NYA_INTERNAL b8 _nya_cheatsheet_next_line(_NYA_CheatReader* reader, OUT char* out, u64 capacity);

/** Collapses runs of whitespace to one space and trims the ends, in place. */
NYA_INTERNAL void _nya_cheatsheet_collapse(char* text);

/** Removes every `__attr_...` and its parenthesised argument, plus a trailing semicolon. */
NYA_INTERNAL void _nya_cheatsheet_strip_attributes(char* text);

/**
 * Removes comments from one line, carrying block comment state across lines through `in_comment`.
 *
 * Text inside a string literal is not exempt, so a declaration holding a slash pair inside quotes
 * would lose the rest of its line. None does; the default string values in the tree are paths.
 * */
NYA_INTERNAL void _nya_cheatsheet_uncomment(char* text, OUT b8* in_comment);

/** Appends `line` to `entry`, separated by one space. */
NYA_INTERNAL void _nya_cheatsheet_append(OUT char* entry, u64 capacity, NYA_ConstCString line);

/** True once the text holds a `;` outside any parenthesis or brace, which ends a declaration. */
NYA_INTERNAL b8 _nya_cheatsheet_is_complete(NYA_ConstCString text);

/** True if `name` was already listed for this header; records it when it was not. */
NYA_INTERNAL b8 _nya_cheatsheet_was_seen(_NYA_CheatFile* file, NYA_ConstCString name);

/** Writes one entry plus its summary into `out`. */
NYA_INTERNAL void _nya_cheatsheet_emit(NYA_String* out, NYA_ConstCString entry, NYA_ConstCString summary);

/** Everything the scanner does with one header. */
NYA_INTERNAL void _nya_cheatsheet_scan_file(NYA_Arena* arena, NYA_String* out, NYA_ConstCString path);

/**
 * Consumes a comment that starts on `line` and keeps its first sentence when the comment is a doc
 * comment short enough to reproduce whole.
 * */
NYA_INTERNAL void _nya_cheatsheet_read_comment(_NYA_CheatReader* reader, _NYA_CheatFile* file, NYA_ConstCString line);

/** Records the first sentence of `prose` as the pending summary, or clears it when it is too long. */
NYA_INTERNAL void _nya_cheatsheet_set_summary(_NYA_CheatFile* file, NYA_ConstCString prose);

/** Joins a declaration that starts on `line` and runs until its terminating semicolon. */
NYA_INTERNAL void _nya_cheatsheet_read_declaration(_NYA_CheatReader* reader, NYA_ConstCString line, OUT char* entry, u64 capacity);

/** The `@file` block's first prose line, or an empty string. Read from the top of the header. */
NYA_INTERNAL void _nya_cheatsheet_file_blurb(NYA_ConstCString text, u64 length, OUT char* out, u64 capacity);

/* CONSTANTS */

NYA_INTERNAL const _NYA_CheatModule _NYA_CHEATSHEET_MODULES[] = {
    { "nyangine-std/base",       "base",     "Arenas, strings, arrays, logging, errors, hashing, files, commands, clocks. No SDL." },
    { "nyangine-core/core",      "core",     "The application loop: entities, systems, events, input, audio, assets, config, saves." },
    { "nyangine-std/math",       "math",     "Scalars, vectors, matrices, quaternions, shapes, noise, random, springs and tweens."     },
    { "nyangine-core/renderer",  "renderer", "2D and 3D drawing, cameras, text, particles, post processing and render targets."        },
    { "nyangine-ui",             "ui",       "Immediate mode widgets: panels, rows, buttons, sliders, toggles and focus navigation."   },
    { "nyangine-core/physics",   "physics",  "Box2D and Box3D behind one interface: bodies, shapes, queries and a character controller." },
    { "nyangine-core/net",       "net",      "The wire: an encrypted session to a peer over UDP, Steam's relay or a loopback pair."  },
    { "nyangine-core/replicate", "replicate", "A world on the wire: commands, delta snapshots, prediction, lag compensation, chat." },
    { "nyangine-core/http",      "http",     "An HTTP/1.1 server, its router and layers, JWT auth, and OpenAPI generated from both."    },
    { "nyangine-std/serde",      "serde",    "One dynamic value type, serialized to and from json, jsonc and the engine's own format."  },
    { "nyangine-core/crypto",    "crypto",   "Hashes, MACs, AEAD, X25519, Ed25519, Argon2id and base32, over monocypher and its vectors." },
    { "nyangine-core/nn",        "nn",       "Tensors, layers, optimizers, DQN and NEAT. A library above math and nothing else."        },
    { "nyangine-core/permission", "permission", "Who may do what to which thing: roles, ranks, overwrites, one resolver, one audit."  },
    { "nyangine-core/db",        "db",       "One database file: bound statements, a reflected struct as a row, derived migrations." },
    { "nyangine-core/debug",     "debug",    "The overlay, the trace, the crash window, and drawing physics shapes and networks."        },
    { "nyangine-plugins",        "plugins",  "Optional dependencies behind a flag: curl, lua, discord, steam, oidc, pgp, and the ACME client."  },
    { "nyangine-std/platform",   "platform", "What the host is, and how to talk to it: signals, the terminal and ipc."                 },
    { "nyangine-std/os",         "os",       "The syscalls themselves: files, pages, the two clocks, random bytes, processes."          },
};

/* PUBLIC API IMPLEMENTATION */

void nya_cheatsheet_generate(void) {
    NYA_ConstCString inputs[]  = { NYA_CHEATSHEET_DIRECTORY, "./src/nyangine-build/pp/cheatsheet.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_CHEATSHEET_OUTPUT, nullptr };
    if (nya_pp_is_current("generate_cheatsheet", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "cheatsheet_generate");
    defer      nya_arena_destroy(arena);

    NYA_String* out = nya_string_create(arena);

    nya_string_extend(out,
                      "<!-- GENERATED by src/nyangine-build/pp/cheatsheet.c from the headers under src/nyangine. DO NYAT EDIT. -->\n\n"
                      "# nyangine cheatsheet\n\n"
                      "Every public declaration in the engine, one line each, in the order its header writes it.\n"
                      "Attributes (`__attr_no_discard`, `__attr_overloaded`, ...) are stripped; nothing else is.\n"
                      "The prose after `//` is the first sentence of the declaration's doc comment, kept only when it\n"
                      "fits whole. The header is the manual: this file is the index into it.\n\n"
                      "Anything spelled `_nya_` or `_NYA_`, or marked `NYA_INTERNAL`, is private and not listed.\n\n"
                      "See also the [prose](README.md) for why things are shaped the way they are, and the "
                      "[doxygen reference](doxygen/html/index.html) for what each declaration does. All three are "
                      "assembled into one deployable tree by `./build docs`.\n\n");

    u32 module_count = (u32)(sizeof(_NYA_CHEATSHEET_MODULES) / sizeof(_NYA_CHEATSHEET_MODULES[0]));

    // the table of contents
    nya_string_extend(out, "## Modules\n\n");
    for (u32 i = 0; i < module_count; i++) {
        nya_string_extend_sprintf(out, "- [`%s`](#%s) — %s\n", _NYA_CHEATSHEET_MODULES[i].title, _NYA_CHEATSHEET_MODULES[i].title,
                                  _NYA_CHEATSHEET_MODULES[i].blurb);
    }
    nya_string_extend(out, "\n");

    u64 header_count = 0;

    for (u32 i = 0; i < module_count; i++) {
        const _NYA_CheatModule* module = &_NYA_CHEATSHEET_MODULES[i];

        NYA_String* directory = nya_string_sprintf(arena, "%s/%s", NYA_CHEATSHEET_DIRECTORY, module->directory);

        NYA_ArrayᐸNYA_Stringᐳ* headers = nya_array_create(arena, NYA_String);
        NYA_EXPECT(nya_filesystem_walk(arena, nya_string_to_cstring(arena, directory), _nya_cheatsheet_collect, headers),
                   "while listing the %s headers", module->title);

        nya_array_sort(headers, _nya_cheatsheet_compare);

        nya_string_extend_sprintf(out, "## %s\n\n%s\n\n", module->title, module->blurb);

        nya_array_foreach (headers, header) {
            _nya_cheatsheet_scan_file(arena, out, nya_string_to_cstring(arena, header));
            header_count++;
        }
    }

    NYA_EXPECT(nya_file_write(NYA_CHEATSHEET_OUTPUT, out), "while writing the cheatsheet");

    nya_log_info("nya_cheatsheet_generate: %llu headers into %s.", (unsigned long long)header_count, NYA_CHEATSHEET_OUTPUT);
}

/* PRIVATE API IMPLEMENTATION */

b8 _nya_cheatsheet_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* headers = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!nya_string_ends_with(entry->name, ".h")) return true;

    // The module aggregate headers (base.h, core.h, ...) are include lists and declare nothing, and render_internal.h is the renderer's private surface despite living beside the public ones.
    if (nya_string_ends_with(entry->name, "render_internal.h")) return true;

    NYA_String* full = nya_string_from(headers->arena, path);
    nya_array_push_back(headers, *full);

    return true;
}

s32 _nya_cheatsheet_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

b8 _nya_cheatsheet_next_line(_NYA_CheatReader* reader, OUT char* out, u64 capacity) {
    nya_assert(reader != nullptr);
    nya_assert(out != nullptr);
    nya_assert(capacity > 1);

    if (reader->offset >= reader->length) return false;

    u64 start = reader->offset;
    u64 end   = start;
    while (end < reader->length && reader->text[end] != '\n') end++;

    reader->offset = end < reader->length ? end + 1 : reader->length;

    // trim both ends before copying, so every caller sees the same shape.
    while (start < end && isspace((unsigned char)reader->text[start])) start++;
    while (end > start && isspace((unsigned char)reader->text[end - 1])) end--;

    u64 length = end - start;
    if (length >= capacity) length = capacity - 1;

    nya_memcpy(out, reader->text + start, length);
    out[length] = '\0';

    return true;
}

void _nya_cheatsheet_collapse(char* text) {
    nya_assert(text != nullptr);

    u64 write = 0;
    b8  space = false;

    for (u64 read = 0; text[read] != '\0'; read++) {
        if (isspace((unsigned char)text[read])) {
            space = true;
            continue;
        }

        if (space && write > 0) text[write++] = ' ';
        space           = false;
        text[write++]   = text[read];
    }

    text[write] = '\0';
}

void _nya_cheatsheet_strip_attributes(char* text) {
    nya_assert(text != nullptr);

    for (;;) {
        char* found = strstr(text, "__attr");
        if (found == nullptr) break;

        char* after = found;
        while (*after != '\0' && (isalnum((unsigned char)*after) || *after == '_')) after++;

        if (*after == '(') {
            u32 depth = 0;
            while (*after != '\0') {
                if (*after == '(') depth++;
                if (*after == ')') {
                    depth--;
                    after++;
                    if (depth == 0) break;
                    continue;
                }
                after++;
            }
        }

        // the space before the attribute goes with it, or the signature keeps a double space.
        while (found > text && found[-1] == ' ') found--;

        memmove(found, after, strlen(after) + 1);
    }

    u64 length = strlen(text);
    while (length > 0 && (text[length - 1] == ';' || text[length - 1] == ' ')) text[--length] = '\0';
}

void _nya_cheatsheet_uncomment(char* text, OUT b8* in_comment) {
    nya_assert(text != nullptr);
    nya_assert(in_comment != nullptr);

    u64 write = 0;
    u64 read  = 0;

    while (text[read] != '\0') {
        if (*in_comment) {
            if (text[read] == '*' && text[read + 1] == '/') {
                *in_comment = false;
                read += 2;
                continue;
            }

            read++;
            continue;
        }

        // a line comment runs to the end of the line, so there is nothing after it to keep.
        if (text[read] == '/' && text[read + 1] == '/') break;

        if (text[read] == '/' && text[read + 1] == '*') {
            *in_comment = true;
            read += 2;
            continue;
        }

        text[write++] = text[read++];
    }

    text[write] = '\0';

    while (write > 0 && isspace((unsigned char)text[write - 1])) text[--write] = '\0';
}

void _nya_cheatsheet_append(OUT char* entry, u64 capacity, NYA_ConstCString line) {
    nya_assert(entry != nullptr);
    nya_assert(line != nullptr);

    u64 used = strlen(entry);
    if (used > 0 && used + 1 < capacity) entry[used++] = ' ';

    u64 length = strlen(line);
    if (used + length >= capacity) length = capacity - used - 1;

    nya_memcpy(entry + used, line, length);
    entry[used + length] = '\0';
}

b8 _nya_cheatsheet_is_complete(NYA_ConstCString text) {
    nya_assert(text != nullptr);

    s32 depth = 0;
    for (u64 i = 0; text[i] != '\0'; i++) {
        if (text[i] == '(' || text[i] == '{' || text[i] == '[') depth++;
        if (text[i] == ')' || text[i] == '}' || text[i] == ']') depth--;
        if (text[i] == ';' && depth <= 0) return true;
    }

    return false;
}

b8 _nya_cheatsheet_was_seen(_NYA_CheatFile* file, NYA_ConstCString name) {
    nya_assert(file != nullptr);
    nya_assert(name != nullptr);

    for (u32 i = 0; i < file->seen_count; i++) {
        if (strcmp(file->seen[i], name) == 0) return true;
    }

    if (file->seen_count >= NYA_CHEATSHEET_MAX_SEEN) return true;

    (void)snprintf(file->seen[file->seen_count], sizeof(file->seen[0]), "%s", name);
    file->seen_count++;

    return false;
}

void _nya_cheatsheet_emit(NYA_String* out, NYA_ConstCString entry, NYA_ConstCString summary) {
    nya_assert(out != nullptr);
    nya_assert(entry != nullptr);

    if (entry[0] == '\0') return;

    if (summary != nullptr && summary[0] != '\0') {
        nya_string_extend_sprintf(out, "%s  // %s\n", entry, summary);
        return;
    }

    nya_string_extend_sprintf(out, "%s\n", entry);
}

void _nya_cheatsheet_set_summary(_NYA_CheatFile* file, NYA_ConstCString prose) {
    nya_assert(file != nullptr);
    nya_assert(prose != nullptr);

    file->summary[0] = '\0';

    // A sentence ends at ". ", or at the end of the comment. Anything with a fence, a tag or a newline in it is a paragraph, not a one-liner, and is left to the header.
    u64 length = 0;
    while (prose[length] != '\0') {
        if (prose[length] == '.' && (prose[length + 1] == '\0' || prose[length + 1] == ' ')) {
            length++;
            break;
        }
        length++;
    }

    if (length == 0 || length > NYA_CHEATSHEET_MAX_SUMMARY) return;
    if (prose[0] == '@' || prose[0] == '`') return;

    nya_memcpy(file->summary, prose, length);
    file->summary[length] = '\0';

    // A one word comment is the declaration's own name written twice; several headers open a struct with its own name as the doc comment. Restating the name beside the name teaches nobody.
    if (strchr(file->summary, ' ') == nullptr) {
        file->summary[0] = '\0';
        return;
    }

    // markdown lives inside a code fence here, so a stray backtick pair is harmless, but a line break would end the entry. There are none after the collapse; assert rather than assume.
    nya_assert(strchr(file->summary, '\n') == nullptr, "a summary must be one line");
}

void _nya_cheatsheet_read_comment(_NYA_CheatReader* reader, _NYA_CheatFile* file, NYA_ConstCString line) {
    nya_assert(reader != nullptr);
    nya_assert(file != nullptr);
    nya_assert(line != nullptr);

    b8 is_doc = strncmp(line, "/**", 3) == 0;

    char prose[NYA_CHEATSHEET_MAX_ENTRY];
    prose[0] = '\0';

    // The terminator is looked for two characters in, not three. A comment written as slash star star slash is closed and empty, but its terminator overlaps the three characters the doc test just matched: searching past them finds none and swallows the rest of the file. nyangine.h has one, used as a separator between include groups.
    const char* opening     = line + (is_doc ? 3 : 2);
    const char* terminating = strstr(line + 2, "*/");

    char buffer[NYA_CHEATSHEET_MAX_LINE];

    b8 closed = terminating != nullptr;

    if (closed) {
        u64 length = terminating > opening ? (u64)(terminating - opening) : 0;
        if (length >= sizeof(buffer)) length = sizeof(buffer) - 1;

        nya_memcpy(buffer, opening, length);
        buffer[length] = '\0';
    } else {
        (void)snprintf(buffer, sizeof(buffer), "%s", opening);
    }

    _nya_cheatsheet_append(prose, sizeof(prose), buffer);

    while (!closed && _nya_cheatsheet_next_line(reader, buffer, sizeof(buffer))) {
        char* terminator = strstr(buffer, "*/");
        if (terminator != nullptr) {
            *terminator = '\0';
            closed      = true;
        }

        // the leading `*` of a continuation line is decoration, not text.
        char* text = buffer;
        while (*text == '*' || *text == ' ') text++;

        _nya_cheatsheet_append(prose, sizeof(prose), text);
    }

    if (!is_doc) {
        file->summary[0] = '\0';
        return;
    }

    _nya_cheatsheet_collapse(prose);
    _nya_cheatsheet_set_summary(file, prose);
}

void _nya_cheatsheet_read_declaration(_NYA_CheatReader* reader, NYA_ConstCString line, OUT char* entry, u64 capacity) {
    nya_assert(reader != nullptr);
    nya_assert(line != nullptr);
    nya_assert(entry != nullptr);

    char buffer[NYA_CHEATSHEET_MAX_LINE];
    b8   in_comment = false;

    // Comments come out line by line rather than off the finished entry: a struct's fields carry doc comments between them, and cutting the joined text at the first one would take the rest of the struct with it.
    (void)snprintf(buffer, sizeof(buffer), "%s", line);
    _nya_cheatsheet_uncomment(buffer, &in_comment);

    entry[0] = '\0';
    _nya_cheatsheet_append(entry, capacity, buffer);

    while (!_nya_cheatsheet_is_complete(entry) && _nya_cheatsheet_next_line(reader, buffer, sizeof(buffer))) {
        _nya_cheatsheet_uncomment(buffer, &in_comment);

        if (buffer[0] == '\0') continue;

        // a preprocessor line inside a struct body is not part of its shape.
        if (buffer[0] == '#') continue;

        _nya_cheatsheet_append(entry, capacity, buffer);
    }

    _nya_cheatsheet_collapse(entry);
    _nya_cheatsheet_strip_attributes(entry);
}

void _nya_cheatsheet_file_blurb(NYA_ConstCString text, u64 length, OUT char* out, u64 capacity) {
    nya_assert(text != nullptr);
    nya_assert(out != nullptr);

    out[0] = '\0';

    _NYA_CheatReader reader = { .text = text, .length = length };

    char line[NYA_CHEATSHEET_MAX_LINE];
    b8   in_file_block = false;

    while (_nya_cheatsheet_next_line(&reader, line, sizeof(line))) {
        if (!in_file_block) {
            if (strncmp(line, "/**", 3) == 0) continue;
            if (strncmp(line, "* @file", 7) == 0) {
                in_file_block = true;
                continue;
            }
            // the first thing that is not the opening of a @file block means there is no blurb.
            if (line[0] != '\0') return;
            continue;
        }

        if (strncmp(line, "*/", 2) == 0 || strcmp(line, "* */") == 0) return;

        char* prose = line;
        while (*prose == '*' || *prose == ' ') prose++;

        if (*prose == '\0') continue;

        // a code fence, an annotation or a heading is not a one line summary.
        if (strncmp(prose, "```", 3) == 0 || *prose == '@' || *prose == '#') return;

        u64 prose_length = strlen(prose);
        if (prose_length + 1 >= capacity) return;

        // A line introducing something rather than saying anything: most headers open their block with "Example:" and then a fence. The fence is caught above, the label would not be.
        if (prose[prose_length - 1] == ':') return;

        (void)snprintf(out, capacity, "%s", prose);
        return;
    }
}

void _nya_cheatsheet_scan_file(NYA_Arena* arena, NYA_String* out, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(out != nullptr);
    nya_assert(path != nullptr);

    NYA_String* text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(path, text), "while reading %s", path);

    _NYA_CheatFile file = {
        .types     = nya_string_create(arena),
        .macros    = nya_string_create(arena),
        .functions = nya_string_create(arena),
    };

    _NYA_CheatReader reader = { .text = (const char*)text->items, .length = text->length };

    char line[NYA_CHEATSHEET_MAX_LINE];
    char entry[NYA_CHEATSHEET_MAX_ENTRY];

    while (_nya_cheatsheet_next_line(&reader, line, sizeof(line))) {
        if (line[0] == '\0') continue;

        if (strncmp(line, "/*", 2) == 0) {
            _nya_cheatsheet_read_comment(&reader, &file, line);
            continue;
        }

        if (strncmp(line, "//", 2) == 0) continue;

        // macros
        if (strncmp(line, "#define ", 8) == 0) {
            const char* cursor = line + 8;

            char name[128];
            u64  name_length = 0;
            while (cursor[name_length] != '\0' && (isalnum((unsigned char)cursor[name_length]) || cursor[name_length] == '_')) {
                if (name_length + 1 >= sizeof(name)) break;
                name[name_length] = cursor[name_length];
                name_length++;
            }
            name[name_length] = '\0';

            b8 emit = name[0] != '_' && (strncmp(name, "nya_", 4) == 0 || strncmp(name, "NYA_", 4) == 0);
            if (emit) emit = !_nya_cheatsheet_was_seen(&file, name);

            // the parameter list, when there is one. `#define X(a, b) ...` versus `#define X 256`.
            entry[0]       = '\0';
            const char* at = cursor + name_length;
            if (*at == '(') {
                u32 depth = 0;
                u64 span  = 0;
                while (at[span] != '\0') {
                    if (at[span] == '(') depth++;
                    if (at[span] == ')') {
                        depth--;
                        span++;
                        if (depth == 0) break;
                        continue;
                    }
                    span++;
                }
                (void)snprintf(entry, sizeof(entry), "%s%.*s", name, (s32)span, at);
            } else {
                // an object macro carries its value only when the value is a short literal on this line. A multi-line body is a code block, and pasting it here would be noise.
                char value[NYA_CHEATSHEET_MAX_LINE];
                (void)snprintf(value, sizeof(value), "%s", at);

                b8 value_in_comment = false;
                _nya_cheatsheet_uncomment(value, &value_in_comment);
                _nya_cheatsheet_collapse(value);

                u64 value_length = strlen(value);
                b8  is_literal   = value_length > 0 && value_length <= NYA_CHEATSHEET_MAX_MACRO_VALUE && value[value_length - 1] != '\\';

                if (is_literal) {
                    (void)snprintf(entry, sizeof(entry), "%s %s", name, value);
                } else {
                    (void)snprintf(entry, sizeof(entry), "%s", name);
                }
            }

            if (emit) _nya_cheatsheet_emit(file.macros, entry, file.summary);
            file.summary[0] = '\0';

            // a macro body continues while the line ends in a backslash, and none of it is a declaration. Without this the scanner reads a macro's guts as code.
            char continuation[NYA_CHEATSHEET_MAX_LINE];
            (void)snprintf(continuation, sizeof(continuation), "%s", line);
            while (strlen(continuation) > 0 && continuation[strlen(continuation) - 1] == '\\') {
                if (!_nya_cheatsheet_next_line(&reader, continuation, sizeof(continuation))) break;
            }

            continue;
        }

        if (line[0] == '#') continue;

        // functions and globals
        if (strncmp(line, "NYA_API ", 8) == 0) {
            _nya_cheatsheet_read_declaration(&reader, line + 8, entry, sizeof(entry));
            _nya_cheatsheet_emit(file.functions, entry, file.summary);
            file.summary[0] = '\0';
            continue;
        }

        // types
        b8 is_aggregate = strncmp(line, "struct NYA_", 11) == 0 || strncmp(line, "enum NYA_", 9) == 0 || strncmp(line, "union NYA_", 10) == 0;

        if (is_aggregate || strncmp(line, "typedef ", 8) == 0) {
            _nya_cheatsheet_read_declaration(&reader, line, entry, sizeof(entry));

            // `typedef struct NYA_Arena NYA_Arena;` names a type the header defines further down. Listing both would say the same thing twice, so the forward declaration is dropped.
            b8 is_forward = strchr(entry, '{') == nullptr && strncmp(entry, "typedef ", 8) == 0 &&
                            (strncmp(entry + 8, "struct ", 7) == 0 || strncmp(entry + 8, "enum ", 5) == 0 || strncmp(entry + 8, "union ", 6) == 0);

            if (!is_forward) _nya_cheatsheet_emit(file.types, entry, file.summary);
            file.summary[0] = '\0';
            continue;
        }

        // anything else is a private declaration, an include or a banner, and carries no summary forward into whatever comes next.
        file.summary[0] = '\0';
    }

    if (file.types->length == 0 && file.macros->length == 0 && file.functions->length == 0) return;

    // the section
    NYA_ConstCString name = strrchr(path, '/');
    name                  = name != nullptr ? name + 1 : path;

    nya_string_extend_sprintf(out, "### %s\n\n", name);

    char blurb[NYA_CHEATSHEET_MAX_LINE];
    _nya_cheatsheet_file_blurb((const char*)text->items, text->length, blurb, sizeof(blurb));
    if (blurb[0] != '\0') nya_string_extend_sprintf(out, "%s\n\n", blurb);

    nya_string_extend(out, "```c\n");

    if (file.types->length > 0) {
        nya_string_extend(out, "// types\n");
        nya_string_extend(out, file.types);
    }
    if (file.macros->length > 0) {
        if (file.types->length > 0) nya_string_extend(out, "\n");
        nya_string_extend(out, "// macros\n");
        nya_string_extend(out, file.macros);
    }
    if (file.functions->length > 0) {
        if (file.types->length > 0 || file.macros->length > 0) nya_string_extend(out, "\n");
        nya_string_extend(out, "// functions\n");
        nya_string_extend(out, file.functions);
    }

    nya_string_extend(out, "```\n\n");
}
