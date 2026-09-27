# Writing a plugin

A plugin is a folder of Lua that somebody else wrote. Dropping it into `plugins/` is the whole
install: no edit in the engine or the game. `plugins/hello/` is a working one, loaded by gnyame at
startup, and the files below are its files.

```
plugins/<name>/
  manifest.nya      who wrote it, and what it wants to touch
  main.lua          the entry point, run last
  src/              every .lua in it runs first, in name order
  assets/           the plugin's own files
  plugin.sig        the signature, written by ./build plugin sign
```

## The manifest

```
nya 2 0
{
    name: string "hello";
    version: string "0.1.0";
    engine_version: string "0.0.0";
    author: string "nyangine";
    license: string "MIT";
    permissions: string[] ["NYA_PLUGIN_PERMISSION_ENTITIES", "NYA_PLUGIN_PERMISSION_INPUT"];
}
```

Every key is a field of `NYA_PluginManifest`, read through reflection, so a key that names no field
refuses the plugin and says which. `name` must be the folder's own name: the directory is the
identity, and two plugins cannot claim one.

## The hooks

```lua
function on_load()
    state.action = nya.input.action_from_name("spawn_burst")
    state.marker = nya.entity.spawn({ name = "hello_plugin_marker", x = 0.0, y = 3.0, z = 0.0 })
end

function on_unload()
    if state.marker ~= nil then nya.entity.despawn(state.marker) end
end

function on_tick(delta_time_s)
    if nya.input.action_just_pressed(state.action) then state.bursts = state.bursts + 1 end
end
```

`on_load`, `on_unload`, `on_frame`, `on_tick` and `on_render`, all optional, run in the same phases
as the engine's own systems. Each plugin is one entry in the system registry under its own name, so
the debug overlay shows what it costs beside the engine and the game.

Every `nya.*` function is listed in `docs/lua/nya.lua`, generated from the headers on every build.
Point an editor at it and it completes them.

## Permissions

The game decides once, at compile time, what any plugin may reach:

| `-DNYA_PLUGIN_PERMISSION_PROFILE=` | Grants |
| :--- | :--- |
| `0` locked | Logging and the clock |
| `1` ui, the default | UI and input state, change nothing |
| `2` gameplay | UI, key bindings, entities, audio, assets |
| `3` all | The above, the plugin's own directory and the network |

A plugin whose manifest asks for more than the build grants is refused at load, with a line naming
each permission it did not get. There is no prompt and no override file, since each of those is a
way for the answer to end up yes.

A denied call is not a call that refuses. It is a name that was never put in the VM, so a script
cannot reach it by asking twice. Each plugin also gets a VM of its own, with no `io`, `os`,
`package`, `ffi` or `debug`, and an instruction budget and heap ceiling, so `while true do end` is
an error and not a hung frame.

## Signing

A build refuses an unsigned plugin by default. The host pins who may sign:

```c
NYA_TRY(nya_plugin_trust_key("nyangine", &nyangine_publisher));
NYA_TRY(nya_plugin_load_all());
```

and the author signs the code and manifest:

```bash
./build plugin keygen --seed plugin_signing.seed       # prints the public key to pin
./build plugin sign plugins/hello --seed plugin_signing.seed
```

Editing a signed plugin invalidates its signature, on purpose. While writing one, compile with
`-DNYA_PLUGIN_REQUIRE_SIGNATURE=false`, which loads it anyway and logs a loud line saying so.

## Next

- `examples/plugin_scripting`: the layer underneath, one Lua VM with a C function registered into
  it, no plugin host.
- `src/nyangine-core/core/core_plugin.h` for what the permission model does not guarantee.
