# Packaging

Every channel ships the same single executable `./build build release` produces, assets included.

| Directory       | Channel                                | Consumes                                   |
| --------------- | -------------------------------------- | ------------------------------------------ |
| `linux/`        | release archive, per-user `install.sh` | the Linux binary                           |
| `aur/`          | Arch User Repository, `gnyame-bin`     | `gnyame.<version>.linux-x86_64.tar.gz`     |
| `flatpak/`      | Flathub, every other distribution      | `gnyame.<version>.linux-x86_64.tar.gz`     |
| `windows/`      | winget and scoop                       | `gnyame.<version>.windows-x86_64.zip`      |
| `steam/`        | SteamPipe, Linux and Windows depots    | both Steam builds                          |

For another game, change `name` in `dist.sh`, `render.sh` and `steam/upload.sh`, `flatpak_id` in
`render.sh`, and the name, description, urls and identifiers in the manifests. Rename the files to match.

## Cutting a release

1. Bump `VERSION` in `src/build/flags.h` and commit.
2. Tag and push: `git tag v1.2.3 && git push origin v1.2.3`.
3. CD checks the tag against `VERSION`, builds, runs `dist.sh` and publishes the GitHub release: both
   binaries, the Linux archive, the Windows zip, both Steam depots, `gnyame.<version>.manifests.tar.gz`
   and `SHA256SUMS`.

The manifests archive holds every manifest below with the version, urls and checksums filled in.
`packaging/dist.sh` after `./build build release` stages the same files locally in `dist/`.

## AUR

```sh
git clone ssh://aur@aur.archlinux.org/gnyame-bin.git
cp manifests/aur/gnyame-bin/{PKGBUILD,.SRCINFO} gnyame-bin/
cd gnyame-bin && makepkg -si && git commit -am "update to 1.2.3" && git push
```

There is no source package: `./build` cross builds every Windows vendor on a Linux host, so it would
need mingw-w64, git-lfs, clang 22 and all seventeen submodules to package a binary the release already has.

## Flathub

Submit `manifests/flatpak/` once as a new app per the Flathub submission guide, then update the url and
`sha256` in the app's Flathub repository on every release. Test locally with
`flatpak-builder --user --install build io.github.lsck0.gnyame.yml`.

## winget

```sh
wingetcreate submit manifests/winget
```

The manifests go to `manifests/l/lsck0/gnyame/<version>/` in `microsoft/winget-pkgs`.

## scoop

Copy `manifests/scoop/gnyame.json` into a bucket repository's `bucket/` directory and push. Its
`autoupdate` section also lets the bucket's excavator pick up new releases on its own.

## Steam

```sh
./build build release
./build build steam-linux
packaging/steam/verify-linux.sh gnyame.<version>.steam-linux-x86_64
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
