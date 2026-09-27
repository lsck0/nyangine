# Screens and scenes

A screen is an arrangement of a window's layer stack: the main menu, the game with its HUD over it,
the pause menu over both. Changing screen is pushing and popping layers. A scene is the other half,
the world those layers draw, written to a file and read back.

gnyame is the complete program here: `src/gnyame/screens.c` for the screens, and the pause menu for
all of it running together.

## Changing screen

```c
void gny_screen_request(GNY_Screen screen) {
    nya_sim_defer(_gny_screen_apply, &screen, sizeof(screen));
}

NYA_INTERNAL void _gny_screen_apply(void* data) {
    switch (*(GNY_Screen*)data) {
        case GNY_SCREEN_PAUSE: {
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU);
            nya_physics2d_enabled_set(false);
        } break;

        case GNY_SCREEN_RESUME: {
            (void)nya_layer_pop(GNY_WINDOW_MAIN);
            nya_physics2d_enabled_set(true);
        } break;
        // ...
    }
}
```

A button asks for a screen, it does not change one. The request is most often made from inside a
layer's `on_update`, while the stack is being walked, and popping the layer that is running pulls
the floor out from under it. `nya_sim_defer` copies the request and applies it at the next barrier,
where nothing is iterating.

gnyame also pops only when the top layer is the one it expects (`nya_layer_get` and the top's `id`),
so a stale request cannot remove another screen's layer.

## Covering the swap

```c
nya_transition_begin(NYA_TRANSITION_FADE_IN, 0.5F, NYA_COLOR_BLACK, NYA_EASE_EXPO_OUT);
```

Begun at the swap itself. An `_IN` transition starts covered and clears, so the new screen is never
shown uncovered for a frame; an `_OUT` covers first, and the swap waits for
`nya_transition_active()` to go false. The overlay is one rectangle drawn by the app loop, with no
shader of its own, and costs nothing when idle.

## Saving the world

```c
NYA_EXPECT(nya_scene_save(nya_world(), "slot0.nya", NYA_SAVE_FLAGS_DATA));

// later, or in another run: everything the world holds is despawned first
NYA_EXPECT(nya_scene_load(nya_world(), "slot0.nya", NYA_SAVE_FLAGS_DATA));

nya_entity_foreach (entity) {
    switch (entity->type) {
        case MY_ENTITY_CRATE: my_crate_attach(entity); break;   // the body, the callbacks
        default:              break;
    }
}
```

A scene carries every entity's identity, transform, hierarchy, flags, type, name, appearance and
light. It does not carry callbacks, rigid bodies or running tweens, because none of them can be
written to a file honestly: a callback handle means nothing the next time the program is laid out in
memory, and a body belongs to a solver that no longer exists. So behaviour is the game's to
re-attach, by `type`, after the load.

`NYA_SAVE_FLAGS_DATA` writes the file obfuscated and checks it on read, so a file edited outside the
game is refused rather than half loaded. Paths are relative to the save root, `~/.local/share/<app_id>`
or `%APPDATA%\<app_id>`.

## Saving less than a world

Most saves are a handful of values, not a world. `nya_save_write` takes any `NYA_Object`, and
`nya_save_version` reads back the version it was written with:

```c
NYA_Object* save = nya_object_create(scratch);
nya_object_add(save, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
nya_object_add(save, "depth", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 41 });

NYA_EXPECT(nya_save_write("progress.nya", save, NYA_SAVE_FLAGS_DATA));
```

`src/gnyame/robots.c` saves its trained brain this way, and drops a file of an older version rather
than guessing at it.

## Next

- [Drawing a UI](drawing-a-ui.md) for the menus a screen is made of.
- `src/nyangine-core/core/core_scene.h` for why a scene is written from its own record type rather
  than from `NYA_Entity`.
