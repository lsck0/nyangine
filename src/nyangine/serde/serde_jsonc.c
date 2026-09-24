#include "nyangine/nyangine.h"

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_String* nya_serde_jsonc_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) {
    // Deliberately the JSON writer: emitting comments would make every reader need a JSONC parser to get the data back. See serde_jsonc.h.
    return nya_serde_json_serialize(arena, object, flags);
}

NYA_Error nya_serde_jsonc_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    return _nya_serde_json_deserialize_with(arena, data, size, flags, true, out_object);
}
