#include "nyangine-core/console/console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_logging.h"

// TYPES

typedef struct _NYA_ConsoleEntry {
    char             name[NYA_CONSOLE_MAX_NAME];
    NYA_ConstCString description;
    NYA_ConsoleFn    handler;
} _NYA_ConsoleEntry;

// PRIVATE STATE

NYA_INTERNAL _NYA_ConsoleEntry _NYA_CONSOLE[NYA_CONSOLE_MAX_COMMANDS] = { 0 };
NYA_INTERNAL u32               _NYA_CONSOLE_COUNT                     = 0;

// PRIVATE API

/** ASCII lower-case, so matching is case-insensitive without a locale. */
NYA_INTERNAL char _nya_console_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/** The registry slot named `name`, or -1. */
NYA_INTERNAL s32 _nya_console_find(NYA_ConstCString name) {
    for (u32 i = 0; i < _NYA_CONSOLE_COUNT; i++) {
        if (strcmp(_NYA_CONSOLE[i].name, name) == 0) return (s32)i;
    }
    return -1;
}

/**
 * A fuzzy subsequence score of `query` against `text`, or a negative when `query` is not a subsequence.
 * Contiguous matches and matches at a word start (text start, or after a space/_/-) score higher, so a
 * tight, word-aligned hit ranks above a scattered one.
 * */
NYA_INTERNAL s32 _nya_console_fuzzy(NYA_ConstCString query, NYA_ConstCString text) {
    if (query[0] == '\0') return 0;

    s32 score    = 0;
    s32 run      = 0;
    u32 t        = 0;
    b8  boundary = true;

    for (u32 q = 0; query[q] != '\0'; q++) {
        char want = _nya_console_lower(query[q]);

        b8 found = false;
        for (; text[t] != '\0'; t++) {
            b8 at_boundary = boundary || t == 0;

            if (_nya_console_lower(text[t]) == want) {
                score += 1;
                if (run > 0) score += 3;         // contiguous with the previous match
                if (at_boundary) score += 5;     // at a word start
                run += 1;
                boundary = (text[t] == ' ' || text[t] == '_' || text[t] == '-');
                t += 1;
                found = true;
                break;
            }

            run      = 0;
            boundary = (text[t] == ' ' || text[t] == '_' || text[t] == '-');
        }

        if (!found) return -1;
    }

    return score;
}

/** Tokenises `line` in place into `out[...]`, minimal double-quote support, returns the token count. */
NYA_INTERNAL u32 _nya_console_tokenise(char* line, OUT NYA_ConstCString* out, u32 capacity) {
    u32 count = 0;
    u32 i     = 0;

    while (line[i] != '\0' && count < capacity) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (line[i] == '\0') break;

        if (line[i] == '"') {
            i++;
            out[count++] = &line[i];
            while (line[i] != '\0' && line[i] != '"') i++;
        } else {
            out[count++] = &line[i];
            while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') i++;
        }

        if (line[i] != '\0') line[i++] = '\0';
    }

    return count;
}

// PUBLIC API

NYA_Error nya_console_register(NYA_ConstCString name, NYA_ConstCString description, NYA_ConsoleFn handler) {
    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a console command needs a name");
    if (strlen(name) >= NYA_CONSOLE_MAX_NAME) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "console command name '%s' is too long", name);
    if (handler == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "console command '%s' needs a handler", name);

    s32 existing = _nya_console_find(name);
    if (existing < 0 && _NYA_CONSOLE_COUNT >= NYA_CONSOLE_MAX_COMMANDS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the console registry is full at %u commands", (u32)NYA_CONSOLE_MAX_COMMANDS);
    }

    u32 slot = existing >= 0 ? (u32)existing : _NYA_CONSOLE_COUNT++;
    (void)snprintf(_NYA_CONSOLE[slot].name, sizeof(_NYA_CONSOLE[slot].name), "%s", name);
    _NYA_CONSOLE[slot].description = description != nullptr ? description : "";
    _NYA_CONSOLE[slot].handler     = handler;

    return NYA_OK;
}

void nya_console_unregister(NYA_ConstCString name) {
    if (name == nullptr) return;

    s32 at = _nya_console_find(name);
    if (at < 0) return;

    // shift the rest down so registration order is kept for an empty-query listing.
    for (u32 i = (u32)at; i + 1 < _NYA_CONSOLE_COUNT; i++) _NYA_CONSOLE[i] = _NYA_CONSOLE[i + 1];
    _NYA_CONSOLE_COUNT--;
}

void nya_console_reset(void) { _NYA_CONSOLE_COUNT = 0; }

void nya_console_printf(NYA_ConsoleInvocation* invocation, NYA_ConstCString format, ...) {
    if (invocation == nullptr || invocation->output == nullptr) return;

    char    line[NYA_CONSOLE_MAX_LINE];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    invocation->output(invocation->user, line);
}

NYA_Error nya_console_run_at(u32 index, const NYA_ConstCString* arguments, u32 argument_count, NYA_ConsoleOutput output, void* user) {
    if (index >= _NYA_CONSOLE_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "console command index %u past %u", index, _NYA_CONSOLE_COUNT);

    NYA_ConsoleInvocation invocation = {
        .name           = _NYA_CONSOLE[index].name,
        .argument_count = 0,
        .output         = output,
        .user           = user,
    };
    for (u32 i = 0; i < argument_count && i < NYA_CONSOLE_MAX_ARGUMENTS; i++) invocation.arguments[invocation.argument_count++] = arguments[i];

    _NYA_CONSOLE[index].handler(&invocation);
    return NYA_OK;
}

NYA_Error nya_console_run(NYA_ConstCString line, NYA_ConsoleOutput output, void* user) {
    if (line == nullptr) return NYA_OK;
    if (strlen(line) >= NYA_CONSOLE_MAX_LINE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "console line longer than %u bytes", (u32)NYA_CONSOLE_MAX_LINE);

    char copy[NYA_CONSOLE_MAX_LINE];
    (void)snprintf(copy, sizeof(copy), "%s", line);

    NYA_ConstCString tokens[NYA_CONSOLE_MAX_ARGUMENTS + 1];
    u32              token_count = _nya_console_tokenise(copy, tokens, NYA_CONSOLE_MAX_ARGUMENTS + 1);
    if (token_count == 0) return NYA_OK;

    s32 at = _nya_console_find(tokens[0]);
    if (at < 0) {
        if (output != nullptr) {
            char message[NYA_CONSOLE_MAX_LINE];
            (void)snprintf(message, sizeof(message), "unknown command: %s", tokens[0]);
            output(user, message);
        }
        return nya_error(NYA_ERROR_NOT_FOUND, "unknown console command '%s'", tokens[0]);
    }

    return nya_console_run_at((u32)at, &tokens[1], token_count - 1, output, user);
}

u32 nya_console_count(void) { return _NYA_CONSOLE_COUNT; }

NYA_ConstCString nya_console_name_at(u32 index) { return index < _NYA_CONSOLE_COUNT ? _NYA_CONSOLE[index].name : nullptr; }

NYA_ConstCString nya_console_description_at(u32 index) { return index < _NYA_CONSOLE_COUNT ? _NYA_CONSOLE[index].description : nullptr; }

u32 nya_console_search(NYA_ConstCString query, OUT NYA_ConsoleMatch* out_matches, u32 capacity) {
    if (out_matches == nullptr || capacity == 0) return 0;
    if (query == nullptr) query = "";

    u32 count = 0;
    for (u32 i = 0; i < _NYA_CONSOLE_COUNT && count < capacity; i++) {
        s32 name_score = _nya_console_fuzzy(query, _NYA_CONSOLE[i].name);
        s32 desc_score = _nya_console_fuzzy(query, _NYA_CONSOLE[i].description);

        // a name hit is worth more than the same hit in the description; unmatched is a negative.
        s32 score = name_score >= 0 ? name_score + 10 : desc_score;
        if (score < 0) continue;

        out_matches[count++] = (NYA_ConsoleMatch){ .index = i, .score = score };
    }

    // insertion sort, best score first: the list is small and already near-ordered by registration.
    for (u32 i = 1; i < count; i++) {
        NYA_ConsoleMatch key = out_matches[i];
        u32              j   = i;
        while (j > 0 && out_matches[j - 1].score < key.score) {
            out_matches[j] = out_matches[j - 1];
            j--;
        }
        out_matches[j] = key;
    }

    return count;
}
