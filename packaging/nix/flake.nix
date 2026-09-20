# `nix run github:lsck0/nyangine?dir=packaging/nix` and `nix profile install` off the same derivation
# nixpkgs would take. Committed so a NixOS user needs nothing from us but this directory.
{
  description = "gnyame, a physics sandbox built on nyangine";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # x86_64-linux only, because that is the only Linux release we build.
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system} = {
        gnyame = pkgs.callPackage ./package.nix { };
        default = self.packages.${system}.gnyame;
      };

      apps.${system}.default = {
        type = "app";
        program = "${self.packages.${system}.gnyame}/bin/gnyame";
      };
    };
}
