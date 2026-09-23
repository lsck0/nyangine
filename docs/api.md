# Reading the API

This page is for judging the shape of the API rather than learning to use it. Every snippet is lifted
from a file in this repository, with the path beside it, so anything here can be checked against what
actually compiles. [CHEATSHEET.md](CHEATSHEET.md) is the full list of names and is generated from the
headers; this is the handful of rules those names follow.

The short version: **a call takes one options struct, answers a value, allocates from an arena the
caller named, and refuses rather than guessing.** Everything below is that sentence, applied.

## 1. Options, not argument lists

A constructor takes a designated-initializer struct. The fields a caller does not name are the
defaults, adding one is not a break, and the call site says what each value means.

```c
// examples/pinball3d/main.c
NYA_EntityHandle handle = nya_entity_spawn(
    .name     = name,
    .type     = kind,
    .position = table_point(flat_center),
    .rotation = table_rotation(),
);
```

```c
// src/gnyame/systems/system_movement.c
nya_system_register((NYA_SystemEntry){ .name   = "camera_follow",
                                       .after  = "player_input",
                                       .before = "tween_tick",
                                       .tick   = nya_callback(gny_system_camera_follow_update),
                                       .owner  = GNY_SYSTEM_OWNER });
```

That second one is also the ordering rule: a system says what it must run after, not what number it
is, so inserting one between two others changes one line rather than renumbering a list.

**What to judge:** whether the field names read as a sentence at the call site, and whether a zeroed
struct is a sane default everywhere (it is meant to be: `{ 0 }` is "nothing unusual").

## 2. Errors are values, and the two macros say what to do with one

`NYA_Error` is returned, never thrown and never stored in a global. Two macros cover the two honest
reactions: pass it up, or state that this cannot fail and mean it.

```c
// examples/net_echo/main.c
NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server), "while creating the server transport");
NYA_EXPECT(nya_net_transport_connect(client, address, port), "while connecting");
```

`NYA_TRY` returns the error to the caller with the trace extended; `NYA_EXPECT` panics with the
message. A function that can fail is `__attr_no_discard`, so ignoring one is a compile error rather
than a habit.

**What to judge:** whether the split between "return it" and "die" is drawn where you would draw it,
and whether the message that comes with a failure tells you enough.

## 3. The caller owns the state

Nothing here keeps a registry of your things. A widget that held its own visibility could never be
shown again by the program, so the flag is the caller's:

```c
// src/gnyame/layers/layer_pause_menu.c
NYA_INTERNAL NYA_UIWindowState _GNY_GUILD_WINDOW = { 0 };

if (!nya_ui_window_begin(ui, "guild", guild_window, &_GNY_GUILD_WINDOW)) return;
```

The same rule at a much larger scale: `permission` takes the table, the subject and the resource and
answers; where the table lives is the program's business, which is why the same module serves a
guild in a game and a route on a server.

```c
// src/gnyame/guild.c
b8 gny_guild_may(u64 subject, enum GnyPermission permission) {
    if (GNY_GUILD.permissions == nullptr) return false;

    return nya_permission_has(GNY_GUILD.permissions, subject, GNY_GUILD_SESSION, (NYA_Permission)permission);
}
```

**What to judge:** whether "the caller owns it" ever costs you more than it buys. The engine's bet is
that a hidden registry costs more the first time you need two of something.

## 4. Handles, not pointers, for anything that can go away

```c
// src/nyangine/core/core_types.h
struct NYA_EntityHandle {
    u32 index;
    u32 generation;
};
```

A stale handle answers "not valid" instead of reading freed memory, which is what lets a system hold
one across frames without a lifetime argument. Pointers are for things whose lifetime the caller
already controls — an arena, a window, a table it made.

## 5. Lifetime is the arena's

Allocation says where, not when to free:

```c
// src/nyangine/http/http_router.h — inside one exchange
/**
 * Scratch for this exchange and nothing longer: it is emptied when the exchange ends, so anything
 * a handler wants to keep has to be copied into the response body before it returns.
 * */
NYA_Arena* arena;
```

An arena is named at creation (`nya_arena_create(.name = "http_exchange")`), and that name shows up
in the overlay and in a crash report, so "what is using memory" is answerable without a profiler.

## 6. One description of a type, used by everything

A struct marked `@reflect` gets a table generated for it, and every module that needs to know a
type's shape reads that same table: JSON and `.nya` serialization, the OpenAPI schema, the ORM, the
config file, the debug inspector.

```c
// src/nyangine/debug/debug_metrics.h
// @reflect
/** What QUERY /api/metrics answers: the frame, and what the server itself has done. */
struct NYA_HttpMetricsDto {
    /** Seconds since the epoch when this was measured, so a page can say how stale it is. */
    u64 measured_at_s;
```

The consequence worth judging: a route that answers this type needs no schema written by hand, and
one that takes it needs no parser. The generated document describes what the handler actually
accepts, because both come from the same table.

## 7. Deny by default, declared at the table

A route says what it needs and what it can answer; the router refuses a table that lies before a
single request arrives.

```c
// src/gnyame/web.c
{
 .method             = NYA_HTTP_METHOD_DELETE,
 .path               = GNY_WEB_GUILD_PATH,
 .auth               = NYA_HTTP_AUTH_BEARER,
 .permission         = GNY_PERMISSION_KICK,
 .resource           = GNY_GUILD_SESSION,
 .handler_identified = gny_web_guild_kick,
 .summary            = "Drops a player",
 .statuses           = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED,
                         NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE },
 },
```

`handler_identified` rather than `handler` is how the type system carries the precondition: a route
behind the extractor cannot be given a handler that takes no identity. The permission is resolved
before the handler runs, so a handler behind one cannot be reached unchecked. See
[http.md](http.md) for the whole of that.

## 8. A bound is a `#define` with its reasoning, and it is measurable

```c
// src/nyangine/http/http_cookie.h
/**
 * Pairs one request's `Cookie` header may hold.
 *
 * A browser sends every cookie in scope for the path, so this is a bound on what a page may have
 * accumulated ... Past it the header is refused whole, because picking the first sixteen means the
 * pair that decides the session might be the seventeenth.
 * */
#define NYA_HTTP_MAX_COOKIES 16
```

Fixed-capacity tables register themselves, so how full each one is shows in the overlay and over
HTTP:

```c
// src/nyangine/core/core_system.c
nya_ceiling_register("systems", NYA_SYSTEM_REGISTRY_MAX, &_nya_system_registry.count);
```

**What to judge:** whether you would rather have growth. The engine's answer is that a bound you can
see beats an allocation you cannot, and that refusing at a number you wrote down beats dying at one
you did not.

## 9. The same widget call, several backends

`nya_ui_*` describes widgets; a *presenter* draws them. Component code never learns which one is
installed:

```c
// src/nyangine/ui/ui_present.h
void  (*look_build)(void*, u32 depth, const NYA_UIStyle*, f32 scale, NYA_UILook* out);
f32x2 (*measure)(void*, NYA_UIText, NYA_ConstCString, f32 room, NYA_UIOverflow);
void  (*draw)(void*, NYA_Window*, const NYA_UIWidgetDraw*);
```

Three exist: shapes (GPU and the terminal's cell renderer), a recorder that draws nothing and keeps
the widget stream (which is how UI is tested without a GPU), and a cell presenter that draws a
terminal's idioms — `[ ok ]`, `[x] label`, box-drawing frames.

## 10. Composition: parts are started, not program kinds

A program is not "a game" or "a server". gnyame is a game that also serves HTTP; the example server
serves a page, a JSON API, a WebSocket stream and a second-factor flow from one listener. The same
`nya_ui_*` code runs on a GPU or in a terminal, and the same permission table answers a guild window
and a route.

---

## The examples, in reading order

| Example | Lines | What it shows |
| :--- | ---: | :--- |
| `hello_world` | 13 | the smallest program that links the engine |
| `cli_tool` | 241 | `nya_args` command trees, no window, no SDL |
| `plugin_scripting` | 190 | Lua plugins with compile-time permissions |
| `net_echo` | 304 | transports, an OS-assigned port, a client and server in one process |
| `tui_dashboard` | 601 | the UI in a terminal |
| `pong_multiplayer` | 458 | the app loop, entities, replication |
| `pinball3d` | 643 | 3D, physics, cameras |
| `web_server` | 901 | routers, DTOs, the generated document, static files, WebSockets, TOTP, sessions |

Run any of them with `./build run example <name>`.

## Where the reasoning lives

Each module's umbrella header is its design document: `src/nyangine/<module>/<module>.h` opens with
what the module is for and what it deliberately does not do. The long-form pages are
[architecture.md](architecture.md) for the engine's shape, [http.md](http.md) for the server, and
`TODO.md` at the repository root, which records decisions with the reasoning that produced them.
