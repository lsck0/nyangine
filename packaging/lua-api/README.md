# The Lua plugin API

Everything needed to write a plugin, and nothing that needs compiling. We ship binaries; this is the
one distribution that is source, because it is the source you are meant to read.

| File           | What it is                                                                      |
| -------------- | ------------------------------------------------------------------------------- |
| `nya.lua`      | The whole API, declared and documented. A language server stub, not a library.   |
| `manifest.nya` | An annotated manifest, every field explained. Copy it.                           |
| `example/`     | A plugin that loads, spawns one entity, moves it and cleans up after itself.     |

## Getting completion

```json
// .luarc.json beside your plugin
{
    "runtime.version": "LuaJIT",
    "workspace.library": ["../lua-api/nya.lua"]
}
```

`runtime.version` matters: the engine embeds LuaJIT, so the language is 5.1 plus LuaJIT's extensions,
not 5.4.

## Writing one

A plugin is a directory under `plugins/` with a `manifest.nya` and the entry point it names. The host
calls `on_load` once, `on_frame` every frame and `on_unload` on the way out, and skips any of the three
that the plugin does not define.

A plugin gets its own VM. One that fails to load, or that errors during a frame, is disabled with its
name and the Lua traceback in the log, and the game carries on without it.
