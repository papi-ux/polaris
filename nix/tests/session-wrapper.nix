# Realize the configured wrapper so writeShellApplication runs ShellCheck.
# Stock Gamescope keeps this lint check independent of the patched compositor
# and the full Polaris build. No wrapper, service or stream is started.
{ self, nixpkgs }:
let
  inherit (nixpkgs) lib;
  pkgs = import nixpkgs {
    system = "x86_64-linux";
    config = { allowUnfree = true; cudaSupport = false; };
    overlays = [ self.overlays.default ];
  };
  evaluated = lib.evalModules {
    modules = [{
      options.services.polaris = import ../modules/options.nix { inherit lib pkgs; };
      config.services.polaris = {
        enable = true;
        packageGamescope = pkgs.gamescope;
      };
    }];
  };
  session = import ../modules/session-lib.nix {
    inherit lib pkgs;
    cfg = evaluated.config.services.polaris;
  };
in
session.sessionBin
