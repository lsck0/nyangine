#include <string.h>

#include "nyangine-core/accounts/accounts_throttle.h"
#include "nyangine-core/accounts/accounts_user.h"
#include "nyangine-std/base/base_ceiling.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_hash.h"

// PRIVATE TYPES

/** One counter, keyed by a hash of the folded username rather than the name itself: no username is kept in memory, and a collision just shares a (harmless) wait. */
typedef struct {
    u64 key;

    u32 failures;
    u64 last_failure_s;
    u64 next_attempt_s;
} _NYA_AccountThrottleEntry;

typedef struct {
    _NYA_AccountThrottleEntry entries[NYA_ACCOUNTS_THROTTLE_ENTRIES];

    /** Where a key goes when the table is full and everything is still waiting: one entry, overwritten by whoever lands on it, so a full table still answers. See _nya_account_throttle_entry. */
    _NYA_AccountThrottleEntry overflow;

    u32 count;
    b8  registered;
} _NYA_AccountThrottleTable;

NYA_INTERNAL _NYA_AccountThrottleTable _NYA_ACCOUNT_THROTTLE = { 0 };

// PRIVATE API DECLARATION

/** The key a username has as a counter, or zero when there is nothing to key on. */
NYA_INTERNAL u64 _nya_account_throttle_key(NYA_ConstCString username) __attr_no_discard;

/** The entry for `key`, making one when there is none. Never null: a full table gives up its stalest row. */
NYA_INTERNAL _NYA_AccountThrottleEntry* _nya_account_throttle_entry(u64 key, u64 now_s) __attr_no_discard;

/** The entry for `key` if it already exists, and null otherwise. What a check uses, so asking costs no slot. */
NYA_INTERNAL _NYA_AccountThrottleEntry* _nya_account_throttle_find(u64 key) __attr_no_discard;

/** The wait `failures` wrong answers have earned, in seconds. Doubling, and bounded. */
NYA_INTERNAL u64 _nya_account_throttle_wait_s(u32 failures) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_AccountThrottleVerdict nya_account_throttle_check(NYA_ConstCString username, u32* out_wait_s) {
    nya_assert(out_wait_s != nullptr);

    *out_wait_s = 0;

    if (!_NYA_ACCOUNT_THROTTLE.registered) {
        nya_ceiling_register("account_throttle", NYA_ACCOUNTS_THROTTLE_ENTRIES, &_NYA_ACCOUNT_THROTTLE.count);
        _NYA_ACCOUNT_THROTTLE.registered = true;
    }

    u64 key = _nya_account_throttle_key(username);
    if (key == 0) return NYA_ACCOUNT_THROTTLE_ALLOW;

    u64 now_s = nya_clock_get_timestamp_s();

    const _NYA_AccountThrottleEntry* entry = _nya_account_throttle_find(key);
    if (entry == nullptr || entry->next_attempt_s <= now_s) return NYA_ACCOUNT_THROTTLE_ALLOW;

    *out_wait_s = (u32)(entry->next_attempt_s - now_s);

    return NYA_ACCOUNT_THROTTLE_WAIT;
}

void nya_account_throttle_fail(NYA_ConstCString username) {
    u64 key = _nya_account_throttle_key(username);
    if (key == 0) return;

    u64 now_s = nya_clock_get_timestamp_s();

    _NYA_AccountThrottleEntry* entry = _nya_account_throttle_entry(key, now_s);

    // A counter quiet longer than it would ever wait starts fresh: a month-old mistype is not a word list, and a monotonic count would eventually punish honest people.
    if (entry->failures > 0 && now_s > entry->last_failure_s + NYA_ACCOUNTS_THROTTLE_FORGET_S) entry->failures = 0;

    if (entry->failures < NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS + 32) entry->failures++;

    entry->last_failure_s = now_s;
    entry->next_attempt_s = now_s + _nya_account_throttle_wait_s(entry->failures);
}

void nya_account_throttle_succeed(NYA_ConstCString username) {
    u64 key = _nya_account_throttle_key(username);
    if (key == 0) return;

    _NYA_AccountThrottleEntry* entry = _nya_account_throttle_find(key);
    if (entry == nullptr) return;

    // Cleared rather than freed: the slot is worth keeping for whoever is using it.
    entry->failures       = 0;
    entry->next_attempt_s = 0;
}

void nya_account_throttle_reset(void) {
    // The registration is kept: nya_ceiling_register holds the address of the count, which does not move.
    b8 registered = _NYA_ACCOUNT_THROTTLE.registered;

    nya_memset(&_NYA_ACCOUNT_THROTTLE, 0, sizeof(_NYA_ACCOUNT_THROTTLE));

    _NYA_ACCOUNT_THROTTLE.registered = registered;
}

u32 nya_account_throttle_count(void) {
    return _NYA_ACCOUNT_THROTTLE.count;
}

// PRIVATE API IMPLEMENTATION

u64 _nya_account_throttle_key(NYA_ConstCString username) {
    if (username == nullptr || username[0] == '\0') return 0;

    // Folded, so a counter follows the account rather than the spelling; see accounts_user.h.
    char folded[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };

    if (!nya_account_username_normalize(username, folded, sizeof(folded))) return 0;

    u64 hash = nya_hash_fnv1a(folded, strlen(folded));

    // Zero is how "nothing to key on" is said, so a hash that lands there is nudged off it.
    return hash == 0 ? 1 : hash;
}

_NYA_AccountThrottleEntry* _nya_account_throttle_find(u64 key) {
    for (u32 index = 0; index < _NYA_ACCOUNT_THROTTLE.count; index++) {
        if (_NYA_ACCOUNT_THROTTLE.entries[index].key == key) return &_NYA_ACCOUNT_THROTTLE.entries[index];
    }

    return nullptr;
}

_NYA_AccountThrottleEntry* _nya_account_throttle_entry(u64 key, u64 now_s) {
    _NYA_AccountThrottleEntry* found = _nya_account_throttle_find(key);
    if (found != nullptr) return found;

    if (_NYA_ACCOUNT_THROTTLE.count < NYA_ACCOUNTS_THROTTLE_ENTRIES) {
        _NYA_AccountThrottleEntry* fresh = &_NYA_ACCOUNT_THROTTLE.entries[_NYA_ACCOUNT_THROTTLE.count++];

        nya_memset(fresh, 0, sizeof(*fresh));
        fresh->key = key;

        return fresh;
    }

    // A full table evicts the entry whose wait ended longest ago; all an attacker gains is that pushed-out counter restarting, not blocking real counters.
    _NYA_AccountThrottleEntry* stalest = &_NYA_ACCOUNT_THROTTLE.entries[0];

    for (u32 index = 1; index < NYA_ACCOUNTS_THROTTLE_ENTRIES; index++) {
        if (_NYA_ACCOUNT_THROTTLE.entries[index].next_attempt_s < stalest->next_attempt_s) {
            stalest = &_NYA_ACCOUNT_THROTTLE.entries[index];
        }
    }

    // Unless everything is still waiting: the newcomer waits the longest wait rather than taking a slot, so a real attack gives up nothing.
    if (stalest->next_attempt_s > now_s) {
        _NYA_AccountThrottleEntry* overflow = &_NYA_ACCOUNT_THROTTLE.overflow;

        overflow->key            = key;
        overflow->failures       = NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS;
        overflow->last_failure_s = now_s;
        overflow->next_attempt_s = now_s + NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S;

        return overflow;
    }

    nya_memset(stalest, 0, sizeof(*stalest));
    stalest->key = key;

    return stalest;
}

u64 _nya_account_throttle_wait_s(u32 failures) {
    if (failures <= NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS) return 0;

    u32 over = failures - NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS;

    // Doubling, and stopped at the cap before the shift could run off the end of the type.
    if (over > 30) return NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S;

    u64 wait_s = (u64)NYA_ACCOUNTS_THROTTLE_BASE_WAIT_S << (over - 1);

    return wait_s > NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S ? NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S : wait_s;
}
