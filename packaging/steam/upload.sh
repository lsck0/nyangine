#!/bin/sh
# Uploads the Steam builds through SteamPipe. Run from the repository root after
# `./build dist steam-linux` and `./build dist steam-windows`, as a Steamworks account with build
# upload rights:
#
#   STEAM_USER=builder STEAM_APP_ID=480 STEAM_DEPOT_ID_LINUX=481 STEAM_DEPOT_ID_WINDOWS=482 packaging/steam/upload.sh
#
# STEAM_PREVIEW=1 builds the depot manifests without uploading anything. A finished upload is set live
# on a branch from the Steamworks builds page.
set -eu

name=gnyame
here="$(dirname "$0")"
work=.steam-upload

: "${STEAM_USER:?}" "${STEAM_APP_ID:?}" "${STEAM_DEPOT_ID_LINUX:?}" "${STEAM_DEPOT_ID_WINDOWS:?}"
preview="${STEAM_PREVIEW:-0}"

# From the build system, not from a regex over one of its headers. See src/nyangine-build/dist.c.
version="$(./build version)"
description="${name} ${version} ($(git rev-parse --short HEAD))"

linux_depot=dist/steam-linux
windows_depot=dist/steam-windows

for depot in "${linux_depot}" "${windows_depot}"; do
    [ -d "${depot}" ] || { echo "no ${depot}; run './build dist $(basename "${depot}")' first" >&2; exit 1; }
done

rm -rf "${work}"
mkdir -p "${work}/scripts" "${work}/content" "${work}/output"

# The staged distributions go up whole: the executable, the Steamworks library, LICENSE, CHANGELOG.md
# and the data/ and plugins/ trees. A depot that is a subset of what a player downloads anywhere else
# is a depot that behaves differently for no reason anyone can see.
cp -r "${linux_depot}" "${work}/content/linux"
cp -r "${windows_depot}" "${work}/content/windows"

# steampipe keeps the executable bit only for files uploaded from linux or macos.
chmod 755 "${work}/content/linux/${name}"

for script in app_build depot_build_linux depot_build_windows; do
    sed -e "s|@APP_ID@|${STEAM_APP_ID}|" \
        -e "s|@DEPOT_ID_LINUX@|${STEAM_DEPOT_ID_LINUX}|" \
        -e "s|@DEPOT_ID_WINDOWS@|${STEAM_DEPOT_ID_WINDOWS}|" \
        -e "s|@DESC@|${description}|" \
        -e "s|@PREVIEW@|${preview}|" \
        "${here}/${script}.vdf" > "${work}/scripts/${script}.vdf"
done

steamcmd +login "${STEAM_USER}" +run_app_build "$(pwd)/${work}/scripts/app_build.vdf" +quit
