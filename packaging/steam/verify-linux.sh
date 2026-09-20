#!/bin/sh
# Checks a Steam Linux depot built by `./build build steam-linux` against the Steam Linux Runtime 3.0 (sniper):
# nothing newer than its glibc 2.31, no OpenSSL, libsteam_api.so found through $ORIGIN, and, where docker runs, a start
# inside Valve's sniper platform image without a Steam client.
#
#   packaging/steam/verify-linux.sh dist/steam-linux
set -eu

name=gnyame
depot="${1:?usage: verify-linux.sh <steam-linux directory>}"
binary="${depot}/${name}"
image=registry.gitlab.steamos.cloud/steamrt/sniper/platform:3.0.20260805.254768

fail() {
    echo "verify-linux: $*" >&2
    exit 1
}

[ -x "${binary}" ] || fail "no executable at ${binary}"
[ -f "${depot}/libsteam_api.so" ] || fail "libsteam_api.so is not beside the executable"

readelf -d "${binary}" | grep -q 'RUNPATH.*\[\$ORIGIN\]' || fail "the executable does not search \$ORIGIN"

if readelf -d "${binary}" | grep -E 'NEEDED.*lib(ssl|crypto)\.so'; then
    fail "OpenSSL is a dynamic dependency"
fi

newest="$(objdump -T "${binary}" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
[ "$(printf '%s\n%s\n' "${newest}" GLIBC_2.31 | sort -V | tail -n 1)" = GLIBC_2.31 ] || fail "needs ${newest}, sniper has GLIBC_2.31"
echo "verify-linux: newest glibc symbol ${newest}, no OpenSSL, \$ORIGIN runpath"

if ! docker info > /dev/null 2>&1; then
    echo "verify-linux: no docker, skipping the start inside the runtime"
    exit 0
fi

# SteamAppId stands in for the client having launched the game, so it starts instead of relaunching through Steam.
# There is no GPU in the container, so the run ends at the renderer; everything before it has to work.
log="$(docker run --rm --volume "$(realpath "${depot}"):/game:ro" --env SteamAppId=480 --env HOME=/tmp \
    --env SDL_VIDEO_DRIVER=offscreen --env SDL_AUDIO_DRIVER=dummy "${image}" timeout 30 /game/${name} --server 2>&1 || true)"

echo "${log}" | grep -q 'error while loading shared libraries' && fail "does not load in sniper: ${log}"
echo "${log}" | grep -q 'Steam is unavailable' || fail "did not fall back from a missing Steam client: ${log}"
echo "${log}" | grep -q 'Integrity check failed' && fail "failed its integrity check: ${log}"
echo "verify-linux: starts in ${image} and carries on without Steam"
