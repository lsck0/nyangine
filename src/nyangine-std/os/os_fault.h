/**
 * @file os_fault.h
 *
 * The syscall funnel's lie detector, run backwards: a seeded plan that makes the calls in os_file.h,
 * os_socket.h, os_time.h, os_random.h and os_page.h fail, come up short, flip a bit, drop or hold a
 * datagram, and read a clock the simulation moves. Compiled only into NYA_TESTING builds on Linux; every
 * other build has no interposer at all, so the calls are the syscalls and nothing else.
 *
 *   nya_os_fault_plan_attach / _detach   installs a plan on the calling thread, and takes it off
 *   nya_os_fault_plan_current            the plan this thread runs under, or null
 *   nya_os_fault_time_advance            moves the simulated clocks forward
 *   nya_os_fault_time_skew_wall          moves the simulated wall clock alone, either way
 *   nya_os_fault_datagrams_release       puts every held datagram on the wire now
 *   nya_os_fault_heal                    zeroes every rule, keeping the clocks and the counters
 *   nya_os_fault_decide                  draws the fault for one call, for a layer above with its own seam
 *
 * ```c
 * NYA_OsFaultPlan plan = { .seed = seed, .path_prefix = scratch, .sockets = NYA_OS_FAULT_SOCKETS_ACCEPTED };
 * plan.rules[NYA_OS_FAULT_FILE_WRITE] = (NYA_OsFaultRule){ .probability_ppm = 50'000 };
 * plan.rules[NYA_OS_FAULT_FILE_SYNC]  = (NYA_OsFaultRule){ .fail_call = 3, .fail_kind = NYA_OS_FAULT_IO };
 *
 * nya_os_fault_plan_attach(&plan);
 * defer nya_os_fault_plan_detach();
 * ```
 *
 * ## Per thread, on purpose
 *
 * A plan belongs to the thread that attached it. The simulation drives the HTTP listener, the UDP
 * transports and the file writes from its own thread, so they are faulted; a job worker or a log
 * thread is not, and never sees a simulated clock jump under a deadline it computed from the real one.
 * It also makes a plan's counters a function of one thread's call order, which is what keeps a seed
 * replaying the same faults.
 *
 * ## What is faulted
 *
 * Files are faulted only when their path starts with `path_prefix`, and read, write and sync follow the
 * descriptor that open handed back. Sockets are faulted by how they came to exist (see
 * NYA_OS_FAULT_SOCKETS_*) and only while open, so a descriptor number the kernel reuses is not faulted
 * for its previous owner. Everything else the engine does keeps working: a log file, an asset read, the
 * test runner's own pipes.
 *
 * Every decision is `splitmix(seed, operation, call index)`, the same counter hash the simulation draws
 * from, so adding a call on one operation never shifts the faults on another.
 *
 * Rejected: LD_PRELOAD or a seccomp trap. Both fault every library in the process, SQLite and OpenSSL
 * included, which then fails in ways the engine's own error paths never see; the funnel is already
 * here, so the seam is here.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/os/os_socket.h"

#if defined(NYA_TESTING) && OS_LINUX
#define NYA_OS_FAULT_ENABLED 1
#else
#define NYA_OS_FAULT_ENABLED 0
#endif

/**
 * The name a syscall's real body is defined under. With the interposer it is a private name the wrapper
 * in os_fault.c calls; without it, the public name, so a release build has one function per call.
 * */
#if NYA_OS_FAULT_ENABLED
#define NYA_OS_SYSCALL(name) _nya_os_real_##name
#define NYA_OS_SYSCALL_LINKAGE NYA_INTERNAL
#else
#define NYA_OS_SYSCALL(name) name
#define NYA_OS_SYSCALL_LINKAGE
#endif

// CONSTANTS

/** Parts per million, which is what a rule's probability is out of. */
#define NYA_OS_FAULT_PPM 1'000'000U

/** Descriptors a plan follows. Past what the simulation opens at once by two orders of magnitude. */
#define NYA_OS_FAULT_DESCRIPTOR_MAX 4096

/** Datagrams the delay fault holds at once. A full ring sends the next one straight away. */
#define NYA_OS_FAULT_HELD_MAX 16

/** Longest datagram the delay and flip faults copy. The UDP transport's MTU is 1200, so this is a ceiling it never reaches. */
#define NYA_OS_FAULT_DATAGRAM_MAX 2048

/** Most later datagram calls a held one waits before it is released. Enough to land behind several newer ones. */
#define NYA_OS_FAULT_HOLD_CALLS_MAX 8

/** Where the simulated wall clock starts: 2000-01-01T00:00:00Z, the simulation's own origin. */
#define NYA_OS_FAULT_WALL_ORIGIN_NS (946'684'800ULL * 1'000'000'000ULL)

/** Where the simulated monotonic clock starts. Not zero, so a duration computed against a zero stamp still looks wrong. */
#define NYA_OS_FAULT_MONOTONIC_ORIGIN_NS (1'000ULL * 1'000'000'000ULL)

// TYPES

typedef enum NYA_OsFaultOp      NYA_OsFaultOp;
typedef enum NYA_OsFaultKind    NYA_OsFaultKind;
typedef struct NYA_OsFaultRule  NYA_OsFaultRule;
typedef struct NYA_OsFaultPlan  NYA_OsFaultPlan;

/** One class of call a rule governs. */
enum NYA_OsFaultOp {
    NYA_OS_FAULT_FILE_OPEN,
    NYA_OS_FAULT_FILE_READ,
    NYA_OS_FAULT_FILE_WRITE,
    NYA_OS_FAULT_FILE_SYNC,
    /** nya_os_file_rename and nya_os_file_replace alike: both are the one rename(2). */
    NYA_OS_FAULT_FILE_RENAME,

    NYA_OS_FAULT_SOCKET_ACCEPT,
    NYA_OS_FAULT_SOCKET_CONNECT,
    NYA_OS_FAULT_SOCKET_SEND,
    NYA_OS_FAULT_SOCKET_RECEIVE,
    NYA_OS_FAULT_SOCKET_SEND_TO,
    NYA_OS_FAULT_SOCKET_RECEIVE_FROM,

    NYA_OS_FAULT_RANDOM,
    NYA_OS_FAULT_PAGE_COMMIT,

    /** Not a syscall: the fallible arena path asks here, so a run can starve one allocation. See base_arena.h. */
    NYA_OS_FAULT_ALLOCATION,

    NYA_OS_FAULT_OP_COUNT,
};

/** What a faulted call does instead. Which kinds an operation can take is in os_fault.c's table. */
enum NYA_OsFaultKind {
    NYA_OS_FAULT_NONE,

    /** EIO. */
    NYA_OS_FAULT_IO,
    /** ENOSPC, on anything that could grow the disk. */
    NYA_OS_FAULT_NO_SPACE,
    /** Less than asked: a short read, write, send or receive. Never zero bytes, which would mean end of file. */
    NYA_OS_FAULT_SHORT,
    /** One bit of what was read, received or sent turned over. */
    NYA_OS_FAULT_FLIP,
    /** EAGAIN, as if nothing were ready. */
    NYA_OS_FAULT_WOULD_BLOCK,
    /** ECONNRESET: the connection is broken both ways, and stays broken. */
    NYA_OS_FAULT_RESET,
    /** ECONNREFUSED on a connect. */
    NYA_OS_FAULT_REFUSED,
    /** The call fails outright: an accept, a random draw, a page commit, an allocation. */
    NYA_OS_FAULT_FAIL,
    /** A datagram that never arrives, sent or received. */
    NYA_OS_FAULT_DROP,
    /** A datagram that arrives twice. */
    NYA_OS_FAULT_DUPLICATE,
    /** A datagram held back past later ones, which is also how datagrams get reordered. */
    NYA_OS_FAULT_DELAY,

    NYA_OS_FAULT_KIND_COUNT,
};

/** Which sockets a plan faults, as flags: by how the socket came to exist while the plan was attached. */
enum {
    NYA_OS_FAULT_SOCKETS_DATAGRAM  = 1 << 0,
    /** Connections a listener accepted, and the accept call itself. */
    NYA_OS_FAULT_SOCKETS_ACCEPTED  = 1 << 1,
    /** Connections this side dialled, and the connect call itself. */
    NYA_OS_FAULT_SOCKETS_CONNECTED = 1 << 2,
};

/** How one operation misbehaves. Zero never faults it. */
struct NYA_OsFaultRule {
    /** Chance a call faults, out of NYA_OS_FAULT_PPM. */
    u32 probability_ppm;

    /** Which kinds a random fault picks from, as `1 << NYA_OsFaultKind`. Zero is every kind the operation can take. */
    u32 kinds;

    /** A scripted fault: the call with this one based index fails with `fail_kind`, whatever the probability says. Zero is none. */
    u64             fail_call;
    NYA_OsFaultKind fail_kind;
};

/**
 * Everything a run tells the interposer, and everything the interposer tells it back. Plain data the
 * caller owns; attach it, change any field while attached, read the counters whenever.
 * */
struct NYA_OsFaultPlan {
    u64 seed;

    NYA_OsFaultRule rules[NYA_OS_FAULT_OP_COUNT];

    /** Files under this prefix are faulted. Null faults no file. The plan keeps the pointer, not a copy. */
    const char* path_prefix;

    /** NYA_OS_FAULT_SOCKETS_* flags. */
    u32 sockets;

    /** Both clocks read the plan rather than the host, and sleeping moves them rather than waiting. */
    b8 time_simulated;

    /** What the monotonic clock reads. Starts at NYA_OS_FAULT_MONOTONIC_ORIGIN_NS when left zero. */
    u64 monotonic_ns;

    /**
     * Nanoseconds every monotonic read moves the clock on its own, so a loop polling a deadline ends.
     * Zero freezes it between advances, which a loop like that never escapes.
     * */
    u64 read_step_ns;

    /** Added to the wall clock and to nothing else, so the wall clock can go backwards while the monotonic one cannot. */
    s64 wall_skew_ns;

    /** nya_os_random_bytes answers from the seed: the same keys, nonces and masks every run. */
    b8 random_seeded;

    // written by the interposer

    u64 calls[NYA_OS_FAULT_OP_COUNT];
    u64 injected[NYA_OS_FAULT_OP_COUNT];

    /** A fold of every fault injected, as (operation, call, kind), for the simulation's trace digest. */
    u64 digest;

    u64 random_draws;

    u64 files[NYA_OS_FAULT_DESCRIPTOR_MAX / 64];
    u64 sockets_followed[NYA_OS_FAULT_DESCRIPTOR_MAX / 64];

    /** Datagrams the delay fault holds right now. The datagrams themselves are the thread's, in os_fault.c, so a plan stays small enough for a stack. */
    u32 held_count;
};

// FUNCTIONS

#if NYA_OS_FAULT_ENABLED

/** Installs `plan` on this thread, replacing none: attaching over an attached plan is refused with false. */
NYA_API b8 nya_os_fault_plan_attach(NYA_OsFaultPlan* plan) __attr_no_discard;

/** Takes this thread's plan off, sending what the delay fault still holds first. A no-op with none attached. */
NYA_API void nya_os_fault_plan_detach(void);

NYA_API NYA_OsFaultPlan* nya_os_fault_plan_current(void) __attr_no_discard;

/** Moves both simulated clocks forward by `nanoseconds`. Only the simulation moves them; nothing waits. */
NYA_API void nya_os_fault_time_advance(NYA_OsFaultPlan* plan, u64 nanoseconds);

/** Moves the wall clock alone, by a signed amount: a negative one is the clock being set back. */
NYA_API void nya_os_fault_time_skew_wall(NYA_OsFaultPlan* plan, s64 nanoseconds);

/** Sends every held datagram now, oldest first, which is how a run heals the network before it checks convergence. */
NYA_API void nya_os_fault_datagrams_release(NYA_OsFaultPlan* plan);

/** Zeroes every rule. Clocks, counters, followed descriptors and held datagrams stay. */
NYA_API void nya_os_fault_heal(NYA_OsFaultPlan* plan);

/**
 * The fault this thread's plan picks for one more call of `op`, counted like the interposer's own, or
 * NYA_OS_FAULT_NONE with no plan attached. For a layer that injects on its own seam, such as the arena's
 * fallible path, so its faults replay with the same seed.
 * */
NYA_API NYA_OsFaultKind nya_os_fault_decide(NYA_OsFaultOp op) __attr_no_discard;

#endif // NYA_OS_FAULT_ENABLED
