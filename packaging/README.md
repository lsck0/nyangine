# Packaging

We ship binaries. Nobody compiles this from source to play it, so there is no source package anywhere
here, and every channel below consumes an archive `./build dist` produced.

```sh
./build dist               # every distribution this host can produce
./build dist linux-nixos   # one of them, and whatever it needs built first
```

`dist/` comes out as one directory per target, plus the archives a release publishes and a `SHA256SUMS`
over them:

```
dist/
  linux/  windows/  steam-linux/  steam-windows/  web/   the thing you run
  linux-pacman/  linux-nixos/  linux-flatpak/            the recipe a package manager reads
  windows-winget/  windows-scoop/
  lua-api/                                               what a plugin author needs
  gnyame.<version>.linux-x86_64.tar.gz                   what the release attaches
  ...
  SHA256SUMS
```

Every runnable distribution has the same shape: the executable, whatever that channel needs beside it,
`LICENSE`, `CHANGELOG.md`, and the `data/` and `plugins/` trees from `runtime/`. There is no `assets/`
directory, because a release binary carries its assets inside it.

## One source of truth

`src/build/dist.c`. There are no shell scripts here any more: the two that existed each parsed `VERSION`
out of `src/build/flags.h` with its own copy of one regex. Anything outside the build system that needs
the version runs `./build version`.

The manifests in this directory are **templates**. `@VERSION@`, `@SHA256_LINUX@`, `@SHA256_WINDOWS@` and
`@DATE@` are filled in when they are staged. Editing a rendered file under `dist/` achieves nothing; edit
the template.

| Directory  | Channel                                | Target              |
| ---------- | -------------------------------------- | ------------------- |
| `runtime/` | the `data/` and `plugins/` skeleton     | every runnable one  |
| `linux/`   | desktop entry, icon, per user installer | `linux`             |
| `aur/`     | `makepkg`, and the AUR as `gnyame-bin`  | `linux-pacman`      |
| `nix/`     | `nix profile install`, nixpkgs          | `linux-nixos`       |
| `flatpak/` | Flathub, every other distribution       | `linux-flatpak`     |
| `windows/` | winget and scoop                        | `windows-winget`, `windows-scoop` |
| `lua-api/` | plugin authors                          | `lua-api`           |
| `steam/`   | SteamPipe, both depots                  | `steam-linux`, `steam-windows` |

For another game, change `PROJECT_NAME` in `src/build/flags.h`, `name` in `steam/upload.sh`, and the
names, descriptions, urls and identifiers in the manifests.

## Cutting a release

1. Bump `VERSION` in `src/build/flags.h` and commit.
2. Tag and push: `git tag v1.2.3 && git push origin v1.2.3`.
3. CD checks the tag against `./build version`, runs `./build dist`, and publishes the archives, the
   `SHA256SUMS` and release notes from `./build changelog --release`.

## AUR

```sh
git clone ssh://aur@aur.archlinux.org/gnyame-bin.git
cp dist/linux-pacman/{PKGBUILD,.SRCINFO} gnyame-bin/
cd gnyame-bin && makepkg -si && git commit -am "update to 1.2.3" && git push
```

## Nix

`dist/linux-nixos/` is a `package.nix` and a flake that wraps it. A user needs neither this repository
nor a checkout:

```sh
nix run github:lsck0/nyangine?dir=packaging/nix
```

The derivation is `autoPatchelfHook` over the release binary with SDL's dlopened backends on
`LD_LIBRARY_PATH`. `dontStrip` is set: the integrity stamp covers the debug sections, so a stripped
binary refuses to start.

## Flathub

Submit `dist/linux-flatpak/` once as a new app per the Flathub submission guide, then update the url and
`sha256` in the app's Flathub repository on every release. Test locally with
`flatpak-builder --user --install build io.github.lsck0.gnyame.yml`.

## winget

```sh
wingetcreate submit dist/windows-winget
```

The manifests go to `manifests/l/lsck0/gnyame/<version>/` in `microsoft/winget-pkgs`.

## scoop

Copy `dist/windows-scoop/gnyame.json` into a bucket repository's `bucket/` directory and push. Its
`autoupdate` section also lets the bucket's excavator pick up new releases on its own, which is why
`SHA256SUMS` names files without a `./` prefix: scoop looks them up in it verbatim.

## Steam

```sh
./build dist steam-linux
./build dist steam-windows
packaging/steam/verify-linux.sh dist/steam-linux
STEAM_USER=... STEAM_APP_ID=... STEAM_DEPOT_ID_LINUX=... STEAM_DEPOT_ID_WINDOWS=... packaging/steam/upload.sh
```

`STEAM_PREVIEW=1` checks the depots without uploading. Set the build live on a branch in Steamworks.

The Steam builds link the Steamworks SDK and carry its library beside the executable. They relaunch
through Steam when started outside it and play on without Steam when the client is not running; put a
`steam_appid.txt` with the app id beside a local build to start it without Steam, and never ship one.
`GNY_STEAM_APP_ID` in `src/gnyame/constants.h` is the app id.

The Linux depot targets Steam Linux Runtime 3.0 (sniper). Set the app's Linux launch option to that
runtime in Steamworks. `steam-linux` compiles everything against the sniper SDK sysroot, which it
downloads once into `vendor/steamrt/`, so the binary needs glibc 2.31 and nothing newer. curl uses the
runtime's GnuTLS, so the binary starts inside the runtime and not necessarily on a host whose nettle
is newer. `verify-linux.sh` checks the symbol versions and starts the build in Valve's sniper image.
No SteamStub DRM wrapper: it rewrites the executable, which breaks the integrity stamp and the signature.

## Web

`dist/web/` is a slot, and empty until the wasm target exists. See `src/build/dist.c`.
