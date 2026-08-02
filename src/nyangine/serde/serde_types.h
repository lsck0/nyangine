/**
 * @file serde_types.h
 *
 * The format and flag enums, split out so serde.h, serde_nya.h and serde_json.h can all name them
 * without including each other.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_SerdeFormat NYA_SerdeFormat;
typedef enum NYA_SerdeFlags  NYA_SerdeFlags;

enum NYA_SerdeFormat {
    /** The native format. Lossless, checksummed. */
    NYA_SERDE_FORMAT_NYA,

    /** Standard JSON. Portable, but loses the distinction between the integer and float widths. */
    NYA_SERDE_FORMAT_JSON,

    NYA_SERDE_FORMAT_COUNT,
};

__attr_allow_unused static NYA_ConstCString NYA_SERDE_FORMAT_NAME_MAP[NYA_SERDE_FORMAT_COUNT] = {
    [NYA_SERDE_FORMAT_NYA]  = "nya",
    [NYA_SERDE_FORMAT_JSON] = "json",
};

enum NYA_SerdeFlags {
    NYA_SERDE_NONE = 0,

    /** Indentation and newlines. Without it the output is compact, which is the default. */
    NYA_SERDE_PRETTY = 1 << 0,

    /**
     * Base64 the output and XOR it with a fixed key, so it is not casually editable in a text
     * editor. nya format only.
     *
     * This is obfuscation, not encryption. The key is in the binary. It stops a player from
     * editing a save file with notepad and nothing more; do not use it to protect anything that
     * matters. Conflicts with NYA_SERDE_PRETTY.
     * */
    NYA_SERDE_OBFUSCATE = 1 << 1,

    /**
     * Do not verify the checksum when reading. Use for data that is expected to be edited by hand,
     * where a mismatch is normal rather than evidence of corruption.
     * */
    NYA_SERDE_NO_CHECKSUM = 1 << 2,

    /* internal */

    /** Set while writing array elements, whose type is already named by the array header. */
    _NYA_SERDE_NO_TYPE_SPECIFIER = 1 << 3,
};
