/**
 * @file base_watch.h
 *
 * What the program's variables held, so a crash report says more than where it stopped.
 *
 * Two things live here, because both answer that question and both have to agree on how a `u8` and a
 * `NYA_String*` are written down:
 *
 * ```c
 * // the operands of a failed comparison, printed with it
 * nya_assert_eq(written, expected);   // ASSERTION FAILED: written == expected, where written is 6 and expected is 8
 *
 * // and the locals of a function that asked to be watched; see build/pp/watch.h for the annotation
 * u32 nya_watch_count(void);
 * const NYA_WatchEntry* nya_watch_at(u32 index);
 * ```
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Why not DWARF
 *
 * The debug information already describes every local of every frame, and reading it is not the same
 * job as reading a variable. A DWARF location is an expression evaluated against the register context
 * of *that* frame, so it needs the unwinder to hand back a full register set per frame and a little
 * stack machine to run the expression in; `DW_OP_fbreg` alone means resolving the frame base first. On
 * top of that the locations are honest about optimisation: in a release build a variable lives in a
 * register for part of a range, nowhere for the rest, and is often gone altogether, which is exactly
 * the build a crash report comes from. So the engine writes down what it wants to read instead, and
 * pays for it only where somebody asked.
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** How much of a string is written down. A crash report wants to recognise a value, not to hold all of it. */
#define NYA_WATCH_STRING_MAX 64

/** Longest rendered value, terminator included: the string above, its quotes, its length and an address. */
#define NYA_WATCH_VALUE_MAX 192

/**
 * Entries one thread's ring holds.
 *
 * A watched frame registers a handful of locals, so 64 is about eight nested watched frames, which is
 * deeper than any chain worth annotating. It is a per thread array of about 3 KiB rather than something
 * that grows: the walk runs after the heap may already be broken, and a ring that allocated to grow
 * would fail exactly when it is read. Past the bound the oldest entry is dropped and the report says
 * how many went, because the innermost frames are the ones that crashed.
 * */
#define NYA_WATCH_RING_MAX 64

// TYPES

typedef enum NYA_WatchType     NYA_WatchType;
typedef struct NYA_WatchEntry  NYA_WatchEntry;

/**
 * How a value is written down. Deliberately coarser than NYA_Type: the width comes from `sizeof` at the
 * site that recorded it, so one arm covers every unsigned integer the engine has.
 * */
enum NYA_WatchType {
    NYA_WATCH_TYPE_SIGNED,
    NYA_WATCH_TYPE_UNSIGNED,
    NYA_WATCH_TYPE_FLOAT,
    NYA_WATCH_TYPE_BOOL,
    NYA_WATCH_TYPE_CHAR,

    /** A `char*`: the text, quoted and cut at NYA_WATCH_STRING_MAX. */
    NYA_WATCH_TYPE_CSTRING,
    /** A `NYA_String*`: the same, plus how long it really is. */
    NYA_WATCH_TYPE_STRING,

    NYA_WATCH_TYPE_POINTER,
    /** Anything else: a struct, a vector, a union. Its address and its size, which is all that is known. */
    NYA_WATCH_TYPE_OPAQUE,

    NYA_WATCH_TYPE_COUNT,
};

/**
 * One live variable. Every string is a literal the compiler emitted, so the entry stays readable after
 * the frame it describes is gone; `address` is the one field that does not, which is what
 * nya_watch_frame_end is for.
 * */
struct NYA_WatchEntry {
    /** Which call registered it. Frames are grouped by this, so recursion reads as two frames. */
    u32 frame;

    NYA_ConstCString function;
    NYA_ConstCString name;

    /** The type as it was written in the source: "u32", "const NYA_String*". */
    NYA_ConstCString type_name;

    const void*   address;
    NYA_WatchType type;
    u32           size;
};

// VALUES

/**
 * The arm of NYA_WatchType that fits `value`'s type, as a constant.
 *
 * Every scalar spelling in base_types.h reaches an arm: the b/u/s/f families are the standard types
 * they are typedefs of, and an enum lexes as its compatible integer type. A pointer that is not text
 * falls to the classifier, which is what keeps a struct from being printed as though it were one.
 * */
// clang-format off
#define _nya_watch_type_of(value)                                                                                                                    \
    _Generic((value),                                                                                                                                \
        bool: NYA_WATCH_TYPE_BOOL,                                                                                                                   \
        char: NYA_WATCH_TYPE_CHAR,                                                                                                                   \
        signed char: NYA_WATCH_TYPE_SIGNED,                                                                                                          \
        short: NYA_WATCH_TYPE_SIGNED,                                                                                                                \
        int: NYA_WATCH_TYPE_SIGNED,                                                                                                                  \
        long: NYA_WATCH_TYPE_SIGNED,                                                                                                                 \
        long long: NYA_WATCH_TYPE_SIGNED,                                                                                                            \
        __int128: NYA_WATCH_TYPE_SIGNED,                                                                                                             \
        unsigned char: NYA_WATCH_TYPE_UNSIGNED,                                                                                                      \
        unsigned short: NYA_WATCH_TYPE_UNSIGNED,                                                                                                     \
        unsigned int: NYA_WATCH_TYPE_UNSIGNED,                                                                                                       \
        unsigned long: NYA_WATCH_TYPE_UNSIGNED,                                                                                                      \
        unsigned long long: NYA_WATCH_TYPE_UNSIGNED,                                                                                                 \
        unsigned __int128: NYA_WATCH_TYPE_UNSIGNED,                                                                                                  \
        _Float16: NYA_WATCH_TYPE_FLOAT,                                                                                                              \
        float: NYA_WATCH_TYPE_FLOAT,                                                                                                                 \
        double: NYA_WATCH_TYPE_FLOAT,                                                                                                                \
        long double: NYA_WATCH_TYPE_FLOAT,                                                                                                           \
        char*: NYA_WATCH_TYPE_CSTRING,                                                                                                               \
        const char*: NYA_WATCH_TYPE_CSTRING,                                                                                                         \
        NYA_String*: NYA_WATCH_TYPE_STRING,                                                                                                          \
        const NYA_String*: NYA_WATCH_TYPE_STRING,                                                                                                    \
        default: (__builtin_classify_type(value) == _NYA_WATCH_CLASS_POINTER ? NYA_WATCH_TYPE_POINTER : NYA_WATCH_TYPE_OPAQUE))
// clang-format on

/**
 * Writes what lives at `address` into `buffer` as terminated text, and returns its length.
 *
 * `size` is the variable's own `sizeof`, which is how one arm of NYA_WatchType covers every width. Reads
 * nothing but those `size` bytes, except through a text pointer, and takes no lock and no allocator: the
 * fault path calls this with the heap possibly already broken.
 * */
NYA_API u32 nya_watch_value_format(NYA_WatchType type, u32 size, const void* address, OUT u8* buffer, u32 capacity);

// ASSERTIONS

/**
 * The comparison assertions. Identical to `nya_assert(a == b)` except that the report carries what `a`
 * and `b` held, which is the thing a failed comparison is usually about.
 *
 * ```c
 * nya_assert_eq(written, expected);
 * nya_assert_lt(index, (u32)nya_carray_length(items));
 * ```
 *
 * Each side is evaluated exactly once, so `nya_assert_eq(read(), 4)` reads once. Both sides keep the
 * type they were written with, so a constant may need the cast the comparison itself would have needed:
 * this is the comparison the caller wrote, with the same warnings on it.
 *
 * There is no form that takes a message. The operands' own names are the context, and a message would
 * have to be pasted into the format string beside them; write `nya_assert` where a sentence is wanted.
 * */
#define nya_assert_eq(left, right) _NYA_ASSERT_COMPARE(left, right, ==)
#define nya_assert_ne(left, right) _NYA_ASSERT_COMPARE(left, right, !=)
#define nya_assert_lt(left, right) _NYA_ASSERT_COMPARE(left, right, <)
#define nya_assert_le(left, right) _NYA_ASSERT_COMPARE(left, right, <=)
#define nya_assert_gt(left, right) _NYA_ASSERT_COMPARE(left, right, >)
#define nya_assert_ge(left, right) _NYA_ASSERT_COMPARE(left, right, >=)

// THE RING

/* The live locals of every watched frame, innermost last, one ring per thread: the generated code in src/genyarated/watches writes it and the crash report reads it. Per-thread and unshared, so it needs no lock or allocator. */

/**
 * Opens a frame's group. The returned mark is both the id its locals are grouped by and the depth
 * nya_watch_frame_end unwinds back to.
 * */
NYA_API u32 nya_watch_frame_begin(void) __attr_no_discard;

/**
 * Drops everything registered since the mark, whether this frame registered it or a callee leaked it.
 *
 * Generated as a `defer`, so it runs on every path out of the function. Nothing else guarantees that a
 * pointer into a frame leaves the ring with the frame, and an entry that outlived its frame is a read
 * of somebody else's stack.
 * */
NYA_API void nya_watch_frame_end(u32 frame);

/** Registers one variable into the calling thread's ring. Written by the pass, not by hand. */
NYA_API void
nya_watch_record(u32 frame, NYA_ConstCString function, NYA_ConstCString name, NYA_ConstCString type_name, NYA_WatchType type, u32 size, const void* address);

/** Entries that can still be read, oldest first: index 0 is the outermost frame's first local. */
NYA_API u32 nya_watch_count(void) __attr_no_discard;

/** Entry `index`, or null past the count. Points into the ring, so read it before anything pushes again. */
NYA_API const NYA_WatchEntry* nya_watch_at(u32 index) __attr_no_discard;

/** How many entries the ring dropped to make room. Reported, so a short list never reads as a whole one. */
NYA_API u32 nya_watch_dropped(void) __attr_no_discard;

// INTERNALS

/** What __builtin_classify_type answers for a pointer. Its other codes are not relied on. */
#define _NYA_WATCH_CLASS_POINTER 5

// clang-format off
#define _NYA_ASSERT_COMPARE(left, right, comparison)                                                                                                 \
    do {                                                                                                                                             \
        /* captured first, so a side effect happens once and the report prints the value that was compared */                                         \
        __auto_type _nya_assert_left  = (left);                                                                                                      \
        __auto_type _nya_assert_right = (right);                                                                                                     \
                                                                                                                                                     \
        if (!(_nya_assert_left comparison _nya_assert_right)) {                                                                                       \
            /* inside the failing branch: a passing assertion costs the comparison and nothing else */                                                \
            u8 _nya_assert_left_text[NYA_WATCH_VALUE_MAX];                                                                                            \
            u8 _nya_assert_right_text[NYA_WATCH_VALUE_MAX];                                                                                           \
                                                                                                                                                     \
            (void)nya_watch_value_format(_nya_watch_type_of(_nya_assert_left), (u32)sizeof(_nya_assert_left), &_nya_assert_left,                      \
                                         _nya_assert_left_text, (u32)sizeof(_nya_assert_left_text));                                                  \
            (void)nya_watch_value_format(_nya_watch_type_of(_nya_assert_right), (u32)sizeof(_nya_assert_right), &_nya_assert_right,                   \
                                         _nya_assert_right_text, (u32)sizeof(_nya_assert_right_text));                                                \
                                                                                                                                                     \
            _nya_crash_raise(NYA_CRASH_SOURCE_ASSERT, __FUNCTION__, __FILE__, __LINE__, 0, "%s " #comparison " %s, where %s is %s and %s is %s",      \
                             #left, #right, #left, (NYA_ConstCString)_nya_assert_left_text, #right,                                                   \
                             (NYA_ConstCString)_nya_assert_right_text);                                                                               \
        }                                                                                                                                            \
    } while (0)
// clang-format on

/**
 * Registers the function's own locals, from where it is written to wherever the function returns.
 *
 * The whole call site: what it expands to was generated into one file per source in
 * src/genyarated/watches, which the source includes itself. See build/pp/watch.h for the annotation
 * that puts it there and for what it can reach.
 * */
#define nya_watch(function) _nya_watch_##function()
