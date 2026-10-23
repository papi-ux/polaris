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
  noCuda = pkgs.polaris-stream.override { cudaSupport = false; };
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
  configuredHelper = lib.findFirst
    (package: lib.getName package == "polaris-gamescope-session")
    (throw "Missing configured Gamescope session helper")
    (session "gamescope_stream").packages;
  projectLine = lib.findFirst (lib.hasPrefix "project(Polaris VERSION ")
    (throw "Missing Polaris project version")
    (lib.splitString "\n" (builtins.readFile (self + "/CMakeLists.txt")));
  projectVersion = builtins.head
    (builtins.match ".*VERSION ([0-9]+[.][0-9]+[.][0-9]+).*" projectLine);
  sourceRevision = self.rev or self.dirtyRev or "unknown";
  facts = {
    packageVersionMatchesProject = noCuda.version == projectVersion && withCuda.version == projectVersion;
    revisionTracksSource = noCuda.COMMIT == sourceRevision && withCuda.COMMIT == sourceRevision;
    noHistoricalVersionOverride = !(noCuda ? BUILD_VERSION) && !(noCuda ? BRANCH);
    publisherMatchesProject = builtins.elem
      (lib.cmakeFeature "POLARIS_PUBLISHER_WEBSITE" noCuda.meta.homepage) noCuda.cmakeFlags;
    configuredHelperWinsProfile = (configuredHelper.meta.priority or 5)
      < (noCuda.meta.priority or 5);
    overlayHonorsCudaChoice = builtins.elem (lib.cmakeBool "POLARIS_ENABLE_CUDA" false) pkgs.polaris-stream.cmakeFlags;
    nonCudaDisablesCuda = builtins.elem (lib.cmakeBool "POLARIS_ENABLE_CUDA" false) noCuda.cmakeFlags;
    cudaVariantRetainsCuda = !(builtins.elem (lib.cmakeBool "POLARIS_ENABLE_CUDA" false) withCuda.cmakeFlags);
    nonCudaHasVulkanHeaders = builtins.elem pkgs.vulkan-headers noCuda.buildInputs;
    cudaHasVulkanHeaders = builtins.elem pkgs.vulkan-headers withCuda.buildInputs;
    nonCudaFindsVulkanLoader = lib.hasInfix
      (builtins.unsafeDiscardStringContext "--prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.vulkan-loader ]}") noCuda.postFixup;
    cudaFindsVulkanLoader = lib.hasInfix
      (builtins.unsafeDiscardStringContext "--prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.vulkan-loader ]}") withCuda.postFixup;
    gamescopeSeedKeepsMode = lib.hasInfix "linux_stream_mode = gamescope_stream" gamescopeSeed;
    labwcSeedUsesPrivateMode = lib.hasInfix "linux_stream_mode = headless_stream" labwcSeed;
    seedsLeaveEncoderAutomatic = !(lib.hasInfix "encoder =" gamescopeSeed)
      && !(lib.hasInfix "encoder =" labwcSeed);
  };
in
assert lib.assertMsg (lib.all (value: value) (builtins.attrValues facts))
  ("Polaris GPU defaults failed: " + builtins.toJSON facts);
pkgs.writeText "polaris-gpu-defaults-check" (builtins.toJSON facts + "\n")
