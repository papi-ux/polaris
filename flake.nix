{
  description = "Polaris game stream host — packages, gamescope-polaris, portal, per-user modules";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = lib.genAttrs systems;

      # Do not exclude basename "images" — web UI ships public/images/*.svg.
      # Only drop agent/session scratch and build artifacts at the repo root.
      polarisSrc = lib.cleanSourceWith {
        src = self;
        filter =
          path: type:
          let
            base = baseNameOf path;
            rel = lib.removePrefix (toString self + "/") (toString path);
          in
          !(
            base == "node_modules"
            || base == ".git"
            || base == "result"
            || base == "result-bin"
            || base == "build"
            || base == ".grok"
            || rel == "images"
            || lib.hasPrefix "images/" rel
          );
      };

      mkPkgs =
        system:
        import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true;
            cudaSupport = true;
          };
          overlays = [ self.overlays.default ];
        };
    in
    {
      overlays.default = final: prev: {
        gamescope-polaris = final.callPackage ./nix/packages/gamescope-polaris { };
        gamescope-hdr = final.gamescope-polaris; # back-compat alias
        xdg-desktop-portal-gamescope = final.callPackage ./nix/packages/xdg-desktop-portal-gamescope { };
        polaris-stream = final.callPackage ./nix/packages/polaris-stream {
          polarisSrc = polarisSrc;
          # Respect the importing host's GPU build choice. mkPkgs keeps the
          # standalone flake's existing CUDA-enabled default.
          cudaSupport = final.config.cudaSupport;
        };
      };

      packages = forAllSystems (
        system:
        let
          pkgs = mkPkgs system;
        in
        {
          inherit (pkgs)
            gamescope-polaris
            gamescope-hdr
            xdg-desktop-portal-gamescope
            polaris-stream
            ;
          default = pkgs.polaris-stream;
        }
      );

      nixosModules.polaris = import ./nix/modules/nixos.nix;
      nixosModules.default = self.nixosModules.polaris;
      nixosModules.overlay = {
        nixpkgs.overlays = [ self.overlays.default ];
      };

      checks.x86_64-linux.nixos-module = import ./nix/tests/nixos-module.nix {
        inherit self nixpkgs;
      };

      checks.x86_64-linux.gpu-defaults = import ./nix/tests/gpu-defaults.nix {
        inherit self nixpkgs;
      };

      hjemModules.polaris = import ./nix/modules/hjem.nix;
      hjemModules.default = self.hjemModules.polaris;

      homeModules.polaris = import ./nix/modules/home-manager.nix;
      homeModules.default = self.homeModules.polaris;
    };
}
