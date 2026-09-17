#!/bin/sh
# Uploads the release executables to Steam through SteamPipe. Run from the repository root after
# `./build build release`, as a Steamworks account with build upload rights:
#
#   STEAM_USER=builder STEAM_APP_ID=480 STEAM_DEPOT_ID_LINUX=481 STEAM_DEPOT_ID_WINDOWS=482 packaging/steam/upload.sh
#
# STEAM_PREVIEW=1 builds the depot manifests without uploading anything. A finished upload is set live
# on a branch from the Steamworks builds page.
set -eu

name=gnyame
here="$(dirname "$0")"
work=dist/steam

: "${STEAM_USER:?}" "${STEAM_APP_ID:?}" "${STEAM_DEPOT_ID_LINUX:?}" "${STEAM_DEPOT_ID_WINDOWS:?}"
preview="${STEAM_PREVIEW:-0}"

version="$(sed -n 's/^#define VERSION *"\([^"]*\)".*/\1/p' src/build/flags.h)"
description="${name} ${version} ($(git rev-parse --short HEAD))"

rm -rf "${work}"
mkdir -p "${work}/scripts" "${work}/content/linux" "${work}/content/windows" "${work}/output"

# steampipe keeps the executable bit only for files uploaded from linux or macos.
install -m755 "${name}.${version}.linux-x86_64" "${work}/content/linux/${name}"
install -m644 "${name}.${version}.windows-x86_64.exe" "${work}/content/windows/${name}.exe"

for script in app_build depot_build_linux depot_build_windows; do
    sed -e "s|@APP_ID@|${STEAM_APP_ID}|" \
        -e "s|@DEPOT_ID_LINUX@|${STEAM_DEPOT_ID_LINUX}|" \
        -e "s|@DEPOT_ID_WINDOWS@|${STEAM_DEPOT_ID_WINDOWS}|" \
        -e "s|@DESC@|${description}|" \
        -e "s|@PREVIEW@|${preview}|" \
        "${here}/${script}.vdf" > "${work}/scripts/${script}.vdf"
done

steamcmd +login "${STEAM_USER}" +run_app_build "$(pwd)/${work}/scripts/app_build.vdf" +quit
