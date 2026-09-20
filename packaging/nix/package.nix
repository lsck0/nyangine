# The release archive, not a source build: `./build` cross compiles every Windows vendor from a Linux
# host and would drag mingw-w64, git-lfs, clang 22 and seventeen submodules into the closure to
# produce a binary the release already has. We ship binaries only.
#
# @VERSION@ and @SHA256_LINUX@ are filled in by `./build dist linux-nixos`.
{
  lib,
  stdenv,
  fetchurl,
  autoPatchelfHook,
  makeWrapper,
  openssl,
  vulkan-loader,
  libGL,
  libxkbcommon,
  wayland,
  libdecor,
  xorg,
  alsa-lib,
  libpulseaudio,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "gnyame";
  version = "@VERSION@";

  src = fetchurl {
    url = "https://github.com/lsck0/nyangine/releases/download/v${finalAttrs.version}/gnyame.${finalAttrs.version}.linux-x86_64.tar.gz";
    # base16, not SRI: it is the same digest SHA256SUMS and every other manifest here carries, so one
    # rendered value fits all of them and nobody has to re-encode it.
    sha256 = "@SHA256_LINUX@";
  };

  # The archive carries its contents at its root, so there is no directory to descend into.
  sourceRoot = ".";

  nativeBuildInputs = [
    autoPatchelfHook
    makeWrapper
  ];

  # Linked in, not dlopened: openssl is what curl resolves against.
  buildInputs = [ openssl ];

  # SDL opens its window, audio and input backends with dlopen at runtime, so they are not NEEDED
  # entries autoPatchelfHook could find. They go on the RUNPATH through the wrapper instead.
  runtimeDependencies = [
    vulkan-loader
    libGL
    libxkbcommon
    wayland
    libdecor
    xorg.libX11
    xorg.libXcursor
    xorg.libXext
    xorg.libXfixes
    xorg.libXi
    xorg.libXrandr
    alsa-lib
    libpulseaudio
  ];

  # The integrity stamp covers the debug sections, so anything that rewrites the binary breaks it.
  dontStrip = true;
  dontPatchELF = false;

  installPhase = ''
    runHook preInstall

    install -Dm755 gnyame              "$out/bin/gnyame"
    install -Dm644 gnyame.desktop      "$out/share/applications/gnyame.desktop"
    install -Dm644 gnyame.png          "$out/share/icons/hicolor/48x48/apps/gnyame.png"
    install -Dm644 LICENSE             "$out/share/licenses/gnyame/LICENSE"
    install -Dm644 CHANGELOG.md        "$out/share/doc/gnyame/CHANGELOG.md"

    # The defaults a fresh install copies into the player's data directory on first run. Read only
    # here, because the store is.
    cp -r data    "$out/share/gnyame/data"
    cp -r plugins "$out/share/gnyame/plugins"

    runHook postInstall
  '';

  postFixup = ''
    wrapProgram "$out/bin/gnyame" \
      --prefix LD_LIBRARY_PATH : "${lib.makeLibraryPath finalAttrs.runtimeDependencies}" \
      --set-default GNYAME_SHARE_DIR "$out/share/gnyame"
  '';

  meta = {
    description = "A physics sandbox built on nyangine";
    homepage = "https://github.com/lsck0/nyangine";
    license = lib.licenses.mit;
    mainProgram = "gnyame";
    platforms = [ "x86_64-linux" ];
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
  };
})
