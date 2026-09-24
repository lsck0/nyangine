#include "nyangine/platform/web/web_storage.h"

// The length of `key` up to and not including its terminator, capped at NYA_WEB_STORAGE_KEY_MAX so a
// missing terminator cannot run the scan off the end. Shared by both backends; a key at or past the cap
// is rejected by the callers below rather than truncated.
static u64 _nya_web_storage_key_length(NYA_ConstCString key) {
    u64 length = 0;
    while (length < NYA_WEB_STORAGE_KEY_MAX && key[length] != '\0') length++;
    return length;
}

#if OS_WASM

#include <emscripten/emscripten.h>

/*
 * localStorage when the page has it, an in-module Map when it does not — which is the case under node,
 * where these run headless in CI. The value is base64 so arbitrary bytes survive a string store, and the
 * key is namespaced so this shares an origin with other localStorage users without colliding. All three
 * are wrapped in try/catch: setItem throws on a full or disabled store, and a throw must read as a clean
 * failure, not a trap that takes the module down.
 *
 * clang-format is off across the EM_JS bodies: the braces hold JavaScript, which the formatter reads as C
 * and breaks (splitting `===`/`!==`, reflowing the object literals). The C around them is formatted.
 */
// clang-format off
EM_JS(int, _nya_web_storage_set_js, (const char* key, const unsigned char* value, int size), {
    try {
        var store = (typeof localStorage !== "undefined") ? localStorage
            : (Module.__nyaMem || (Module.__nyaMem = {
                _m: {},
                getItem: function(k) { return (k in this._m) ? this._m[k] : null; },
                setItem: function(k, v) { this._m[k] = v; },
                removeItem: function(k) { delete this._m[k]; }
            }));
        var bin = "";
        for (var i = 0; i < size; i++) bin += String.fromCharCode(HEAPU8[value + i]);
        store.setItem("nya:" + UTF8ToString(key), btoa(bin));
        return 1;
    } catch (e) {
        return 0;
    }
})

EM_JS(int, _nya_web_storage_get_js, (const char* key, unsigned char* out, int capacity), {
    var store = (typeof localStorage !== "undefined") ? localStorage : Module.__nyaMem;
    if (!store) return -1;
    var value = store.getItem("nya:" + UTF8ToString(key));
    if (value === null || value === undefined) return -1;
    var bin = atob(value);
    var length = bin.length;
    var written = (length < capacity) ? length : capacity;
    for (var i = 0; i < written; i++) HEAPU8[out + i] = bin.charCodeAt(i) & 0xff;
    return length;
})

EM_JS(int, _nya_web_storage_delete_js, (const char* key), {
    var store = (typeof localStorage !== "undefined") ? localStorage : Module.__nyaMem;
    if (!store) return 0;
    var namespaced = "nya:" + UTF8ToString(key);
    var existed = store.getItem(namespaced) !== null && store.getItem(namespaced) !== undefined;
    store.removeItem(namespaced);
    return existed ? 1 : 0;
})
// clang-format on

b8 nya_web_storage_set(NYA_ConstCString key, const u8* value, u64 size) {
    if (key == nullptr || _nya_web_storage_key_length(key) >= NYA_WEB_STORAGE_KEY_MAX) return false;
    if (value == nullptr && size != 0) return false;

    return _nya_web_storage_set_js(key, value, (int)size) == 1;
}

s64 nya_web_storage_get(NYA_ConstCString key, OUT u8* buffer, u64 capacity) {
    if (key == nullptr || buffer == nullptr || _nya_web_storage_key_length(key) >= NYA_WEB_STORAGE_KEY_MAX) return -1;

    return (s64)_nya_web_storage_get_js(key, buffer, (int)capacity);
}

b8 nya_web_storage_delete(NYA_ConstCString key) {
    if (key == nullptr || _nya_web_storage_key_length(key) >= NYA_WEB_STORAGE_KEY_MAX) return false;

    return _nya_web_storage_delete_js(key) == 1;
}

#else // native fallback: an in-process table for the run, not persistence — see the header

#include "nyangine/base/base_memory.h"

// A small fixed table, enough for the handful of durable values client state keeps and no arena to own:
// the store outlives every caller and lives for the whole run, so it is file scope. A value past the
// per-entry cap is refused rather than truncated, the way the browser refuses a value past its budget.
#define NYA_WEB_STORAGE_NATIVE_ENTRIES   32
#define NYA_WEB_STORAGE_NATIVE_VALUE_MAX 4096

typedef struct {
    b8  used;
    u8  key[NYA_WEB_STORAGE_KEY_MAX];
    u64 key_length;
    u8  value[NYA_WEB_STORAGE_NATIVE_VALUE_MAX];
    u64 value_length;
} NyaWebStorageEntry;

static NyaWebStorageEntry _nya_web_storage_table[NYA_WEB_STORAGE_NATIVE_ENTRIES];

// The entry holding `key`, or null when nothing does. Length-checked, not string-compared, so a key with
// an embedded terminator is still matched whole.
static NyaWebStorageEntry* _nya_web_storage_find(NYA_ConstCString key, u64 key_length) {
    for (u32 i = 0; i < NYA_WEB_STORAGE_NATIVE_ENTRIES; i++) {
        NyaWebStorageEntry* entry = &_nya_web_storage_table[i];
        if (!entry->used || entry->key_length != key_length) continue;
        if (nya_memcmp(entry->key, key, key_length) == 0) return entry;
    }
    return nullptr;
}

b8 nya_web_storage_set(NYA_ConstCString key, const u8* value, u64 size) {
    if (key == nullptr || (value == nullptr && size != 0) || size > NYA_WEB_STORAGE_NATIVE_VALUE_MAX) return false;

    u64 key_length = _nya_web_storage_key_length(key);
    if (key_length >= NYA_WEB_STORAGE_KEY_MAX) return false;

    NyaWebStorageEntry* entry = _nya_web_storage_find(key, key_length);
    if (entry == nullptr) {
        for (u32 i = 0; i < NYA_WEB_STORAGE_NATIVE_ENTRIES; i++) {
            if (!_nya_web_storage_table[i].used) {
                entry = &_nya_web_storage_table[i];
                break;
            }
        }
        if (entry == nullptr) return false; // the table is full, which the browser store would call a quota refusal
    }

    entry->used       = true;
    entry->key_length = key_length;
    nya_memcpy(entry->key, key, key_length);
    if (size != 0) nya_memcpy(entry->value, value, size);
    entry->value_length = size;

    return true;
}

s64 nya_web_storage_get(NYA_ConstCString key, OUT u8* buffer, u64 capacity) {
    if (key == nullptr || buffer == nullptr) return -1;

    u64 key_length = _nya_web_storage_key_length(key);
    if (key_length >= NYA_WEB_STORAGE_KEY_MAX) return -1;

    NyaWebStorageEntry* entry = _nya_web_storage_find(key, key_length);
    if (entry == nullptr) return -1;

    u64 written = entry->value_length < capacity ? entry->value_length : capacity;
    if (written != 0) nya_memcpy(buffer, entry->value, written);

    return (s64)entry->value_length;
}

b8 nya_web_storage_delete(NYA_ConstCString key) {
    if (key == nullptr) return false;

    u64 key_length = _nya_web_storage_key_length(key);
    if (key_length >= NYA_WEB_STORAGE_KEY_MAX) return false;

    NyaWebStorageEntry* entry = _nya_web_storage_find(key, key_length);
    if (entry == nullptr) return false;

    entry->used = false;
    return true;
}

#endif // OS_WASM
