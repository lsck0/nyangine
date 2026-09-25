#include "nyangine-std/serde/serde_cbor.c"
#include "nyangine-std/serde/serde_dispatch.c"
#include "nyangine-std/serde/serde_json.c"
#include "nyangine-std/serde/serde_jsonc.c"
#include "nyangine-std/serde/serde_nya.c"
// After the text form, whose boolean helpers it shares rather than repeats.
#include "nyangine-std/serde/serde_nya_binary.c"
// After the formats it dispatches through, and after base_reflection.c, whose pair it wraps.
#include "nyangine-std/serde/serde_reflect.c"
