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

## Publishing it as a Tor onion service

`deploy/onion.yaml` publishes the same image as a Tor v3 onion service. It is a two-container compose
stack: the app, and a `tor` daemon whose one job is a `HiddenService` that forwards the onion's virtual
port 80 to the app's `:8000`. The app publishes nothing to the host — there is no `ports:` on the `web`
service — so the only way in is the `.onion`.

**No application change is needed.** Tor reaches the server as an ordinary HTTP peer over the compose
network, and the image already binds the wildcard (`--address 0.0.0.0` in its `ENTRYPOINT`), so the same
binary that faces a published port in the swarm faces the Tor daemon here. The onion is plain HTTP end
to end and that is correct: Tor's own layer is the encryption and the authentication of the address, so
a self-signed TLS certificate on top would only add a warning a visitor has to click through.

```bash
docker compose -f examples/web_server/deploy/onion.yaml up -d
```

### Getting the onion hostname

Tor generates the address on first start and writes it to the `hostname` file inside the HiddenService
directory. Read it out of the running `tor` container:

```bash
docker compose -f examples/web_server/deploy/onion.yaml exec tor cat /var/lib/tor/hidden_service/hostname
# -> <56-character-base32>.onion
```

Open that in Tor Browser. The address is derived from the service's private key, so it is stable as long
as that key is: the `onion_keys` named volume is what keeps it across a restart.

### Pinning a persistent or vanity key

The HiddenService directory holds two files Tor made — `hs_ed25519_secret_key` and `hostname` — and that
secret key **is** the address. To keep an address you already have, restore that directory into the
`onion_keys` volume before `up`; to move the service to another host, copy the volume there. To mint a
*vanity* address (one whose prefix is a word) generate a key pair offline with a tool such as
[`mkp224o`](https://github.com/cathugger/mkp224o), then drop its `hs_ed25519_secret_key` into the volume
and let Tor derive the matching `hostname` on start.

**Never commit an onion private key**, and never bake one into an image: anyone who has it can serve
your address. `.gitignore` the key, inject it as a secret, and treat a leak as a lost address. Nothing
in this repository contains one, and `onion.yaml` keeps the key in a named volume precisely so it never
lands in the build context.

### Binding the mirror attestation to the onion

Once you know the `.onion`, set the `web` service's `WEB_SERVER_ORIGIN` to `http://<hash>.onion` and
inject `WEB_SERVER_ATTESTATION_SEED` as a secret, then redeploy — see the next section for what those do.

## The mirror attestation

The server signs a statement of who it is and what it serves, published at
`/.well-known/mirror-attestation`, so that a mirror of the site can be *proven* to serve the origin's
bytes rather than tampered ones — the thing an onion service wants, where reaching the one origin is slow
and a mirror is a machine someone else runs.

The document is an Ed25519 signature over a canonical, length-prefixed manifest — the origin, a
timestamp, and a SHA-256 digest of the served bundle (the three files hashed with their paths, in mount
order). The signing key is the origin's own, and it is a **configured secret, never checked in**: set
`WEB_SERVER_ATTESTATION_SEED` (32 bytes, base64url) to keep a stable key across restarts, or leave it
unset and the server makes a throwaway pair each start and logs its public half. Pin that public key
wherever a client will verify from.

```bash
# what the origin serves
curl -s http://127.0.0.1:47800/.well-known/mirror-attestation | jq .

# verify a mirror against the pinned origin key: the binary is its own verifier
./web_server.example --verify-mirror http://<mirror-host> --origin-key <base64url public key>
```

`--verify-mirror` fetches the mirror's attestation, checks the signature against the key you pinned (not
the key the document carries — a mirror could present its own), then fetches the bundle from the mirror
and recomputes the digest, so a pass means the mirror served the origin's exact bytes and the origin
signed for them. It runs and exits like `--healthcheck`, starting none of the server.

## Proof of work in front of abuse

An onion service has no client IP to rate-limit: every request arrives from the local Tor daemon, so the
per-address bounds see one address. What is left to charge an abusive client is its own CPU. Run the
server with `--proof-of-work` and the notes resource is walled behind a challenge: a request with no
proof gets `401` with an `X-Pow-Challenge` (a nonce), an `X-Pow-Difficulty`, and a sealed `X-Pow-Token`;
the client finds a suffix whose SHA-256 with the nonce has that many leading zero bits and resends with
`X-Pow-Solution` and the token. A browser clears it unnoticed; a script hammering the route pays for
every request. The challenge is sealed with the server's own key and expires, so a client cannot forge
one or lower its difficulty, and a solved token is single-use. See `src/nyangine/http/http_pow.h`.

Nothing here was launched against a live Tor network from this repository — the onion stack is
configuration, validated for coherence and documented, and brought up with the commands above on a host
that has Docker and can reach the Tor network.
