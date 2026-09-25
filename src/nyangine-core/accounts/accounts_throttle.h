/**
 * @file accounts_throttle.h
 *
 * What a wrong password costs the next one, so guessing is not free.
 *
 * ```c
 * // nya_account_authenticate does this itself; a caller only reads the answer
 * u32 wait_s = 0;
 * if (nya_account_throttle_check("ada", "203.0.113.9", &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT) {
 *     answer_429(wait_s);
 * }
 * ```
 *
 * ── slowed, never locked ──
 *
 * An account is never locked out, because a lockout is a denial of service anybody can perform on
 * anybody: send five wrong passwords for a name and its owner cannot get in. What this does instead is
 * make each wrong answer buy the next attempt a wait — one second, then two, then four, up to
 * NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S — which expires on its own and is cleared the moment a password is
 * right. An attacker gets a handful of guesses an hour; the owner waits a few seconds at worst.
 *
 * ── two counters, and what each is for ──
 *
 * One per address and one per username. The address counter is what stops somebody working through a
 * word list against one account; the username counter is what stops the same list arriving from a
 * hundred addresses, which is what a botnet does and what an address counter alone cannot see.
 *
 * Both are checked *before* the password is hashed, which is the point: Argon2id is 30 ms of this
 * process's CPU, and an attacker who can spend it at will has a denial of service whether or not they
 * ever guess a password.
 *
 * ── what this deliberately leaks ──
 *
 * That an address or a name is being throttled: a refusal inside the wait costs nothing, where a real
 * attempt costs a hash. Whoever is guessing already knows they are guessing, and the alternative —
 * hashing anyway to keep the cost flat — would mean an attacker can make this process do the
 * expensive thing as often as they like. The refusal is still the same sentence nya_account_authenticate
 * gives for every other failure, so nothing here says whether an account exists.
 *
 * ── in memory, not in the database ──
 *
 * The counters are a fixed table in this process. They are about traffic arriving now rather than
 * about an account, so they cost no row, and a restart forgetting them is a restart an attacker cannot
 * cause. A second process has its own table, which is the same trade the HTTP rate limiter makes.
 *
 * Thread safety: none, as with the rest of the module. One thread authenticates.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Failures tracked at once, addresses and usernames together.
 *
 * Fixed, like every other bound the server keeps: the table is full when it is full, and the entry
 * closest to expiring is reused. That is what keeps somebody inventing usernames from being able to
 * make this allocate.
 * */
#define NYA_ACCOUNTS_THROTTLE_ENTRIES 128

/** Wrong answers allowed before a wait starts. The first mistyped password is not a punishment. */
#define NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS 3

/** The wait after the first failure past the free ones, in seconds. It doubles from here. */
#define NYA_ACCOUNTS_THROTTLE_BASE_WAIT_S 1

/** The longest wait, in seconds. Five minutes: long enough to make a word list useless, short enough to sit out. */
#define NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S 300

/** How long an entry with no failures in it is kept before its slot may be taken. */
#define NYA_ACCOUNTS_THROTTLE_FORGET_S 3600

// TYPES

/** What a check answers. */
typedef enum {
    /** Nothing is owed; go on and verify the password. */
    NYA_ACCOUNT_THROTTLE_ALLOW = 0,

    /** Too soon. Refuse without hashing, and say how long is left if the answer is an HTTP one. */
    NYA_ACCOUNT_THROTTLE_WAIT,
} NYA_AccountThrottleVerdict;

// FUNCTIONS

/**
 * Whether this username and this address may try a password right now, and how long is left if not.
 *
 * Either may be null, which is what a login with no address to record looks like: the counter that has
 * nothing to key on is simply not consulted. `username` is folded with nya_account_username_normalize
 * first, so `Ada` and `ada` share a counter the way they share an account.
 * */
NYA_API NYA_AccountThrottleVerdict nya_account_throttle_check(NYA_ConstCString username, NYA_ConstCString address, OUT u32* out_wait_s)
    __attr_no_discard;

/** Records a wrong password, which is what buys the next attempt its wait. */
NYA_API void nya_account_throttle_fail(NYA_ConstCString username, NYA_ConstCString address);

/** Records a right one, which clears both counters: whoever this is, they are who they said. */
NYA_API void nya_account_throttle_succeed(NYA_ConstCString username, NYA_ConstCString address);

/** Forgets everything. What a test calls between cases, and what a program calls when it wants a clean slate. */
NYA_API void nya_account_throttle_reset(void);

/** How many entries are in use, for the ceiling audit and for a server's own metrics. */
NYA_API u32 nya_account_throttle_count(void) __attr_no_discard;
