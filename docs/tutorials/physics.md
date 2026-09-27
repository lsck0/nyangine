# Physics

Physics is a property of an entity. Spawn one, attach a body, and the solver writes the entity's
transform every tick. Nothing in the game steps a world or names Box2D or Box3D: the engine's
physics systems run at the top of every tick, before any layer's `on_update`, and the entity is
where the result shows up.

2D and 3D have the same shape, one prefix apart. The complete 3D program is `examples/pinball3d`;
the 2D one is gnyame itself.

```bash
./build run example pinball3d
```

## Attaching a body

```c
NYA_EntityHandle ball = nya_entity_spawn(.name = "ball", .position = { 0.0F, 1.0F, 0.0F });

b8 attached = nya_physics3d_body_attach(ball,
    .type        = NYA_PHYSICS_BODY_DYNAMIC,
    .shape       = NYA_PHYSICS3D_SHAPE_SPHERE,
    .radius      = 0.075F,
    .density     = 7800.0F,
    .is_bullet   = true,
);
nya_assert(attached);
```

The macro starts from sensible defaults (a dynamic box, some friction, little bounce), so a field
left out is a default and not a zero. The 2D call is the same:
`nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32, 32 })`.

| Type | Moves | Use for |
| :--- | :--- | :--- |
| `NYA_PHYSICS_BODY_STATIC` | Never | Walls, floors, terrain |
| `NYA_PHYSICS_BODY_KINEMATIC` | Where it is told, pushes without being pushed | Moving platforms, doors |
| `NYA_PHYSICS_BODY_DYNAMIC` | By the solver | Everything that falls |

`is_bullet` turns on continuous collision for something small and fast, which otherwise tunnels
through a thin wall between two ticks. `is_sensor` makes a trigger volume that reports and never
pushes.

## Moving one

While a body is attached the solver owns the transform, and writing `entity->position` is
overwritten on the next step. Move a body through the solver instead:
`nya_physics3d_apply_impulse`, `_apply_force`, `_velocity_set`, `_angular_velocity_set`.

`nya_physics3d_teleport` exists for a respawn, and it is the wrong tool for anything that should hit
something. A teleport sets the pose without a sweep, so the body arrives with no speed to give away.
The pinball flippers first moved that way and pushed the ball out from inside rather than throwing
it; driven by their velocity, the contact carries it.

## Hearing what hit what

```c
u32                   hit_count = 0;
const NYA_PhysicsHit* hits      = nya_physics3d_hits(&hit_count);

for (u32 i = 0; i < hit_count; i++) {
    if (hits[i].kind != NYA_PHYSICS_HIT_IMPACT) continue;
    f32 loudness = hits[i].approach_speed / HIT_LOUD_SPEED;
    // ...
}
```

The list is the step that ran at the top of this tick. The two sides of an impact are not ordered,
so check both `a` and `b`. `nya_physics3d_hit_threshold_set` drops impacts slower than a speed,
which is how a rolling ball stops reporting a hit every tick. Sensors report
`NYA_PHYSICS_HIT_SENSOR_ENTER` and `_EXIT`, with the sensor as `a`.

## Layers

```c
NYA_PhysicsLayerMask terrain = nya_physics_layer("terrain");
NYA_PhysicsLayerMask debris  = nya_physics_layer("debris");

nya_physics2d_body_attach(chunk, .size = { 4, 4 }, .layers = debris, .collides_with = terrain | debris);
```

A layer is registered by name on first use, because a save file or a tilemap names "terrain", not
bit 3. A contact survives only when both sides agree to it, and the broadphase rejects the pair
before any narrowphase work. There is no group index: layers are the whole vocabulary.

## Units and pausing

2D works in pixels, `NYA_PHYSICS2D_PIXELS_PER_METER` to the metre; 3D in metres. Densities are real
ones, so steel is 7800 and a ball made of it feels heavy.

`nya_physics2d_enabled_set(false)` freezes the world with everything else running, which is how
gnyame's pause menu pauses the game.

## Next

- `src/nyangine-core/physics/physics2d_controller.h`: `nya_character2d_update`, a platformer
  controller with coyote time and a jump buffer, over a 2D body.
- `src/nyangine-core/physics/physics3d.h` for meshes, heightfields and raycasts.
