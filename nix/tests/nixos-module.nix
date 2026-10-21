# Importing the game host module must work with the ordinary NixOS module set,
# whether the user enables the host or leaves it disabled.
{ self, nixpkgs }:
let
  system = "x86_64-linux";
  host =
    enable:
    nixpkgs.lib.nixosSystem {
      inherit system;
      modules = [
        self.nixosModules.polaris
        self.nixosModules.overlay
        {
          services.polaris.enable = enable;
          system.stateVersion = "26.05";
        }
      ];
    };
in
assert (host true).config.services.polaris.enable;
assert !(host false).config.services.polaris.enable;
nixpkgs.legacyPackages.${system}.writeText "polaris-nixos-module-check" "Module evaluation passed.\n"
