#!/bin/sh
# Installs gnyame for the current user: the binary on PATH, the icon and the desktop entry, so it shows up
# in application launchers. Run from the extracted release archive.
set -eu

here="$(dirname "$0")"
bin="${XDG_BIN_HOME:-$HOME/.local/bin}"
data="${XDG_DATA_HOME:-$HOME/.local/share}"

install -Dm755 "$here/gnyame" "$bin/gnyame"
install -Dm644 "$here/gnyame.png" "$data/icons/hicolor/48x48/apps/gnyame.png"
install -Dm644 "$here/gnyame.desktop" "$data/applications/gnyame.desktop"

echo "installed to $bin/gnyame; make sure $bin is on PATH"
