# Evaluate the actual package and generated seed for both GPU build variants.
# This is a packaging/defaults check, not a GPU or NixOS deployment test.
{ self, nixpkgs }:
let
  system = "x86_64-linux";
  inherit (nixpkgs) lib;
  pkgs = import nixpkgs {
    inherit system;
    config = { allowUnfree = true; cudaSupport = false; };
    overlays = [ self.overlays.default ];
  };
  noCuda = pkgs.polaris-stream;
  withCuda = noCuda.override { cudaSupport = true; };
  session = streamMode:
    let
      evaluated = lib.evalModules {
        modules = [{
          options.services.polaris = import ../modules/options.nix { inherit lib pkgs; };
          config.services.polaris.streamMode = streamMode;
        }];
      };
    in import ../modules/session-lib.nix {
      inherit lib pkgs;
      cfg = evaluated.config.services.polaris;
    };
  gamescopeSeed = (session "gamescope_stream").polarisConfSeed.text;
  labwcSeed = (session "labwc").polarisConfSeed.text;
  facts = {
    nonCudaDisablesCuda = builtins.elem "-DPOLARIS_ENABLE_CUDA=OFF" noCuda.cmakeFlags;
    cudaVariantRetainsCuda = !(builtins.elem "-DPOLARIS_ENABLE_CUDA=OFF" withCuda.cmakeFlags);
    nonCudaHasVulkanHeaders = builtins.elem pkgs.vulkan-headers noCuda.buildInputs;
    cudaHasVulkanHeaders = builtins.elem pkgs.vulkan-headers withCuda.buildInputs;
    nonCudaFindsVulkanLoader = lib.hasInfix
      "--prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.vulkan-loader ]}" noCuda.postFixup;
    cudaFindsVulkanLoader = lib.hasInfix
      "--prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.vulkan-loader ]}" withCuda.postFixup;
    gamescopeSeedKeepsMode = lib.hasInfix "linux_stream_mode = gamescope_stream" gamescopeSeed;
    labwcSeedUsesPrivateMode = lib.hasInfix "linux_stream_mode = headless_stream" labwcSeed;
    seedsLeaveEncoderAutomatic = !(lib.hasInfix "encoder =" gamescopeSeed)
      && !(lib.hasInfix "encoder =" labwcSeed);
  };
in
assert lib.assertMsg (lib.all (value: value) (builtins.attrValues facts))
  ("Polaris GPU defaults failed: " + builtins.toJSON facts);
pkgs.writeText "polaris-gpu-defaults-check" (builtins.toJSON facts + "\n")
