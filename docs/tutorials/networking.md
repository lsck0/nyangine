# Networking

There is no single player mode. A lone player is a server nobody has connected to, and the local
player reaches it through a loopback transport, running exactly the client code a remote player
runs. Opening the game to the network is one call, and the world does not change.

The complete program is `examples/pong_multiplayer`:

```bash
./build run example pong_multiplayer                            # alone
./pong_multiplayer.example --listen 27015                       # host
./pong_multiplayer.example --connect 127.0.0.1 --port 27015     # join, from another terminal
```

## Two halves

| Module | Knows about | Use it for |
| :--- | :--- | :--- |
| `net` | Peers and bytes: encrypted UDP, reliable and unreliable channels | A tool, a relay, a service |
| `replicate` | Entities: snapshots, commands, prediction | A game world |

A program that only moves bytes links `net` and stops there; `examples/net_echo` is that, with no
window and no world. The rest of this page is `replicate`.

## Starting the server

```c
NYA_NetLaunchConfig launch = nya_net_config_from_args(argc, argv);

NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
    .replicated_flag  = FLAG_REPLICATED,
    .on_spawn_player  = nya_callback(pong_spawn_player),
    .on_apply_command = nya_callback(pong_apply_command),
    .max_speed        = PADDLE_SPEED_LIMIT,
}), "while starting the server");

if (launch.listen_port != 0) (void)nya_net_server_listen(launch.listen_port);

NYA_NetTransport* local = nullptr;
NYA_EXPECT(nya_net_server_attach_local(&local));
NYA_EXPECT(nya_net_client_attach(local, launch.name, (NYA_NetClientConfig){
    .replicated_flag   = FLAG_REPLICATED,
    .on_apply_command  = nya_callback(pong_apply_command),
    .on_sample_command = nya_callback(pong_sample_command),
}));
```

`nya_net_config_from_args` parses `--connect`, `--port`, `--listen`, `--server`, `--name`,
`--server-key` and the `--net-latency`/`--net-loss` family, so every nyangine program takes the same
flags. A remote player calls `nya_net_client_connect` with the address instead of attaching.

Only entities carrying `replicated_flag` go on the wire. The flag is the game's choice of bit.

## Commands, and why prediction works

A client never moves its own entity directly. It samples what the player is asking for into a
command, and one function turns a command into movement:

```c
void pong_sample_command(OUT NYA_NetCommand* command) {
    nya_net_command_set(command, COMMAND_BIT(PONG_ACTION_UP), nya_input_action_pressed(PONG_ACTION_UP));
}

void pong_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    if (nya_net_command_holds(command, COMMAND_BIT(PONG_ACTION_UP))) entity->position.y -= PADDLE_SPEED * delta_time_s;
}
```

The client runs `on_apply_command` at once to predict; the server runs the same function with the
same command and the same fixed step, and its snapshot is the answer. Since both ran the same code,
they agree, and a correction is rare. Make movement depend on anything the client does not have (a
clock, a random draw, another entity) and the two diverge every tick.

`max_speed` is the server refusing to trust that function. A command that would move an entity
further is cut short and counted as a violation.

## Drawing between ticks

What the client does not own, it interpolates. Call `nya_net_client_interpolate` before anything
reads a position, then draw `nya_entity_render_position` rather than `entity->position`: the second
is where the last tick left it, the first is where it is this frame.

Branch on `nya_net_server_running()` for authority work, such as the ball in pong, which only the
server integrates. `nya_net_client_stats()` gives round trip time and loss for a HUD.

## Next

- [Adding a system](adding-a-system.md): the server and client ticks are systems named `net_server`
  and `net_client`.
- `src/nyangine-core/replicate/replicate.h` for why the line between `net` and `replicate` is where
  it is.
