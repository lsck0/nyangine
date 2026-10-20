# Container image

`Dockerfile` builds the `web_server` example into an image. The builder is Arch Linux at a pinned digest
with its packages from a dated Arch Linux Archive snapshot, so clang 23 and every library are the same on
each build. It runs `./build run example web_server --server`, which builds the Linux vendors, the shaders
and the asset blob, then the example as a release binary, stripped, with its assets baked in. The runtime
stage is `FROM scratch`: that binary, the shared objects `ldd` names, an empty `/tmp`, and `/data`.

```bash
# from the repository root, submodules checked out; the context is the whole tree, minus .dockerignore
docker build -t nyangine/web_server -f packaging/docker/Dockerfile .

docker run --rm -p 8000:8000 -v web_server_data:/data nyangine/web_server
curl -f http://127.0.0.1:8000/          # the page
curl -f http://127.0.0.1:8000/healthz   # {"status":"ok"}
```

podman builds the same file; pass `--format docker`, since the OCI format drops `HEALTHCHECK`.

## What it runs as

| Setting              | Value                 | Why                                                         |
| :------------------- | :-------------------- | :---------------------------------------------------------- |
| user                 | `65532:65532`         | not root; a scratch image has no `/etc/passwd`, so a number |
| port                 | `8000`                | the one `EXPOSE`d                                            |
| `/data`              | volume, owned by 65532 | `notes.db` lives here; mount a volume to keep it            |
| `XDG_DATA_HOME`      | `/data`               | the save root resolves under it                              |
| `WEB_SERVER_ADDRESS` | `0.0.0.0`             | nothing else in the container to proxy for it                |
| `WEB_SERVER_PORT`    | `8000`                | the server and `--healthcheck` both read it                  |

Address and port are environment variables, so `docker run -e WEB_SERVER_PORT=9000 -p 9000:9000` moves the
port without a rebuild; a flag on the command line still wins over its variable. The other variables
the server reads (`WEB_SERVER_ORIGIN`, `WEB_SERVER_ATTESTATION_SEED`, `WEB_SERVER_OTLP_FILE`) pass through the
same way.

The healthcheck is `/web_server --healthcheck`: the binary GETs `/` on the loopback port through the
HTTP client it already links and exits 0 only on a 200, because a scratch image has no curl.

## Size

Measured on 2026-10-20: **26.7 MB**. Most of it is the stripped binary (about 11 MB) and the libraries it
resolves at runtime: libssl and libcrypto, libstdc++, libz, brotli, zstd, glibc and the loader. No SDL:
the example compiles the whole engine graph, since `core`, `http` and `crypto` still sit behind
`NYA_NO_SDL` in `nyangine.h`, but the release link's `--gc-sections` drops everything the server never
reaches. When the server builds headless (`NYA_SERVER_VENDORS_LINUX_X86_64` in
`src/nyangine-build/vendor/vendor.h`), the builder stops compiling SDL, box2d, box3d, ufbx and shadercross
and the image shrinks further; nothing here changes.

The first build compiles every Linux vendor from source, DXC among them, which takes minutes; later
builds reuse the layer until the tree changes.

## Deploying

`examples/web_server/deploy/` holds the swarm stack and the Tor onion stack that run this image.
