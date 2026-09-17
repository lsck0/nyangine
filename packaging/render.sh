#!/bin/sh
# Writes the package manifests for one release, with its version and the checksums of its archives,
# ready to push to the AUR, a scoop bucket, winget-pkgs and Flathub.
#
#   packaging/render.sh <version> <directory with the release archives> <output directory>
set -eu

name=gnyame
flatpak_id=io.github.lsck0.gnyame

version="$1"
dist="$2"
out="$3"
here="$(dirname "$0")"

linux_sha="$(sha256sum "${dist}/${name}.${version}.linux-x86_64.tar.gz" | cut -d' ' -f1)"
windows_sha="$(sha256sum "${dist}/${name}.${version}.windows-x86_64.zip" | cut -d' ' -f1)"

# every url names the release twice, as the tag and in the file name. [^/$] leaves scoop's $version alone.
url="s#/v[^/\$]+/${name}\.[^/\$]+\.(linux-x86_64\.tar\.gz|windows-x86_64\.zip)#/v${version}/${name}.${version}.\1#"

mkdir -p "${out}/aur/${name}-bin" "${out}/scoop" "${out}/winget" "${out}/flatpak"

sed -E -e "${url}" \
    -e "s/^pkgver=.*/pkgver=${version}/" \
    -e "s/^pkgrel=.*/pkgrel=1/" \
    -e "s/^sha256sums=.*/sha256sums=('${linux_sha}')/" \
    "${here}/aur/${name}-bin/PKGBUILD" > "${out}/aur/${name}-bin/PKGBUILD"
sed -E -e "${url}" \
    -e "s/^(\s*pkgver = ).*/\1${version}/" \
    -e "s/^(\s*pkgrel = ).*/\11/" \
    -e "s/^(\s*sha256sums = ).*/\1${linux_sha}/" \
    "${here}/aur/${name}-bin/.SRCINFO" > "${out}/aur/${name}-bin/.SRCINFO"

sed -E -e "${url}" \
    -e "s/^(    \"version\": ).*/\1\"${version}\",/" \
    -e "s/^(            \"hash\": ).*/\1\"${windows_sha}\"/" \
    "${here}/windows/scoop/${name}.json" > "${out}/scoop/${name}.json"

for manifest in "${here}"/windows/winget/*.yaml; do
    sed -E -e "${url}" \
        -e "s/^(PackageVersion: ).*/\1${version}/" \
        -e "s/^(    InstallerSha256: ).*/\1${windows_sha}/" \
        "${manifest}" > "${out}/winget/$(basename "${manifest}")"
done

sed -E -e "${url}" \
    -e "s/^(        sha256: ).*/\1${linux_sha}/" \
    "${here}/flatpak/${flatpak_id}.yml" > "${out}/flatpak/${flatpak_id}.yml"
sed -E -e "s#<release version=\"[^\"]*\" date=\"[^\"]*\"/>#<release version=\"${version}\" date=\"$(date -u +%Y-%m-%d)\"/>#" \
    "${here}/flatpak/${flatpak_id}.metainfo.xml" > "${out}/flatpak/${flatpak_id}.metainfo.xml"
