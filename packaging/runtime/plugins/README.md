# plugins

One directory per plugin, discovered at startup. The host knows nothing about any of them; adding one
is dropping a directory here, and removing one is deleting it.

```
plugins/
  my_plugin/
    manifest.nya     what it is called, what it needs, where it starts
    main.lua         the entry point named by the manifest
    src/*.lua        everything else it requires
    assets/          its own images, sounds and data
```

`example/` beside this file is a working one; copy it.

## The manifest

`manifest.nya` carries the identity (`id`, `name`, `description`, `author`, `license`, `version`), the
`engine_version` the plugin was written against, its `dependencies` and `conflicts` on other plugins,
and a `repository` git URL.

That URL is how a plugin updates: checking for one is a fetch of refs from the author's own repository.
No registry, no account, nothing central that can go away and take every plugin with it.

A plugin whose `engine_version` is newer than the engine's is refused. One whose dependency is missing,
or which conflicts with another installed plugin, is disabled with both names in the log. None of these
stop the game.

## The API

`nya` is the only global the host provides. `example/` uses it, and the whole surface is listed in
`nya.lua` in the Lua API distribution, which doubles as an editor completion stub: point your language
server at it and you get the signatures while you type.

## Rules

- A plugin gets its own Lua VM. One that fails to load, or errors on a frame, is disabled with its name
  and the Lua traceback in the log; the game carries on.
- `assets/` is addressed relative to the plugin, so two plugins may both have `assets/icon.png`.
- Load order is the manifest's `after` list, then alphabetical. Nothing may depend on the alphabet.
