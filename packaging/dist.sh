#!/bin/sh
# Collects a release into dist/: the executables, a Linux archive with the desktop entry, icon and
# installer, a portable Windows zip, the package manifests for this version, and SHA256SUMS over all
# of it. Run from the repository root after `./build build release`.
set -eu

name=gnyame

version="$(sed -n 's/^#define VERSION *"\([^"]*\)".*/\1/p' src/build/flags.h)"
linux="${name}.${version}.linux-x86_64"
windows="${name}.${version}.windows-x86_64"

rm -rf dist
mkdir -p "dist/stage/${name}" dist/stage/windows
cp "${linux}" "${windows}.exe" dist/

install -m755 "${linux}" "dist/stage/${name}/${name}"
cp "packaging/linux/${name}.desktop" "packaging/linux/${name}.png" packaging/linux/install.sh LICENSE "dist/stage/${name}/"
tar -C dist/stage -czf "dist/${linux}.tar.gz" "${name}"

cp "${windows}.exe" "dist/stage/windows/${name}.exe"
cp LICENSE dist/stage/windows/LICENSE.txt
(cd dist/stage/windows && zip -q -X "../../${windows}.zip" "${name}.exe" LICENSE.txt)

packaging/render.sh "${version}" dist dist/stage/manifests
tar -C dist/stage -czf "dist/${name}.${version}.manifests.tar.gz" manifests
rm -rf dist/stage

# last, over the files as published, so nothing can change between hashing and upload. No ./ prefix,
# scoop's autoupdate looks names up in this file verbatim.
(cd dist && sha256sum -- * > SHA256SUMS)
