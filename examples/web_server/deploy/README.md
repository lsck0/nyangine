# Deploying web_server to a homelab swarm

The `web_server` example ships as a container image and rolls out through the docker-swarm
mechanism in [homelab](https://github.com/lsck0/homelab): a service listens on `:8000` with a
`GET /` that answers 200, an image is pushed to `registry.lsck0.dev/<name>:latest` (or `ghcr.io`),
and the swarm rolls it out start-first, rolling back if the new task never turns healthy.

## The image

`examples/web_server/Dockerfile` is a two-stage build. The builder brings up the clang toolchain the
engine needs (a very recent clang, for the matrix_type extension, `-fdefer-ts` and `_Float16`, plus
lld), bootstraps `./build`, and builds the example as a **release, stripped** artifact with its assets
baked in. The runtime stage is `FROM scratch` and holds only that binary, the shared libraries `ldd`
reports, and a writable `/data`. A measured build is **~39 MB** — the 9.4 MB stripped binary plus
libssl/libcrypto/libz/brotli/zstd/libstdc++ and the loader, and nothing else.

```bash
# from the repository root — the build context is the whole tree, not this directory
docker build -t registry.lsck0.dev/web_server:latest -f examples/web_server/Dockerfile .

# run it locally
docker run --rm -p 8000:8000 registry.lsck0.dev/web_server:latest
curl -f http://localhost:8000/        # 200: the page
curl -f http://localhost:8000/healthz # 200: liveness

# push it where the swarm pulls from
docker push registry.lsck0.dev/web_server:latest
```

### Why the image is not smaller still

The engine is one graph today: `core` (the asset, save, callback and event systems this server uses),
`http` and `crypto` all sit behind `NYA_NO_SDL` in `src/nyangine/nyangine.h`, and `core` embeds the
renderer by value — so the example compiles the whole engine and links the full vendor set, SDL
included. What keeps the image small anyway is the release link: `--gc-sections` drops every function
the server never reaches, so `ldd` on the result shows **no SDL** at all, and `-s` strips the debug
info. When a genuinely headless engine build lands, this same binary links only the server vendor
subset (`NYA_SERVER_VENDORS_LINUX_X86_64` in `src/build/vendor/vendor.h`, which `./build --server`
already knows how to build on its own) and the image shrinks further; nothing here changes.

## The healthcheck

Docker runs a container's healthcheck **inside** the container. homelab's `webService` helper uses
`curl -f http://localhost:8000/`, and a `FROM scratch` image has no curl — so this image and its
stack ask the same question a different way: the server probes itself.

```
/web_server.example --healthcheck --port 8000
```

is a client, not a server: it GETs `/` on the loopback port through the HTTP client the binary already
links and exits 0 only on a 200. The image declares it as its `HEALTHCHECK`, and `deploy/stack.yaml`
sets the same as the compose healthcheck, so the swarm's start-first rollout has a real signal to gate
on. An operator who wants the literal `curl` form can build the image on a base that carries curl and
use homelab's helper unmodified.

## Adding it to the swarm

`deploy/stack.yaml` is the compose stack, already in the shape homelab's `webService` produces. Add it
to a homelab instance as a `homelab.swarm.stacks` entry — mirroring
`src/instances/209-external-hello.nix`:

```nix
{ ... }:
{
  networking.hostName = "vm-210";

  homelab.swarm = {
    enable = true;
    updateInterval = "1m";

    stacks = {
      web-server = ''
        services:
          web:
            image: registry.lsck0.dev/web_server:latest
            ports:
              - "80:8000"
            healthcheck:
              test: ["CMD", "/web_server.example", "--healthcheck", "--port", "8000"]
              interval: 10s
              timeout: 5s
              retries: 3
              start_period: 5s
            deploy:
              replicas: 1
              update_config:
                parallelism: 1
                order: start-first
                failure_action: rollback
              rollback_config:
                order: start-first
              restart_policy:
                condition: any
            volumes:
              - web_server_data:/data
        volumes:
          web_server_data:
      '';
    };

    # A private registry needs credentials; homelab keeps the token in sops:
    # registries."registry.lsck0.dev" = { username = "lsck0"; passwordFile = config.sops.secrets.registry-token.path; };
  };

  networking.firewall.allowedTCPPorts = [ 80 ];
}
```

Push a new `:latest` and the swarm rolls it out within `updateInterval`, start-first: the new task has
to pass its healthcheck before the old one stops, and a task that never turns healthy is rolled back.
