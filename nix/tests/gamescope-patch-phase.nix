# Run gamescope-polaris's own patches and postPatch against whichever nixpkgs
# the flake is evaluated with. The package pins a gamescope revision, so its
# path fixups have to match that source; nixpkgs releases move them between
# files, and an importing host's nixpkgs replaces this flake's lock. CI also
# runs this with nixpkgs overridden to the current stable release.
# Only the sources are fetched: no compiler, meson or gamescope build.
{ self, nixpkgs }:
let
  pkgs = import nixpkgs {
    system = "x86_64-linux";
    overlays = [ self.overlays.default ];
  };
  package = pkgs.gamescope-polaris;
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "gamescope-polaris-patch-phase";
  inherit (package)
    version
    src
    patches
    postPatch
    ;
  # postPatch's patchShebangs resolves interpreters from PATH, as the build does.
  nativeBuildInputs = [ pkgs.python3 ];

  dontConfigure = true;
  dontBuild = true;

  # Not failing is not enough: each fixup must have reached its target.
  installPhase = ''
    runHook preInstall
    grep -F "return \"$out\";" src/Utils/DirHelpers.cpp
    grep -F "(char *)\"$out/bin/gamescopereaper\"" src/Utils/Process.cpp
    for script in subprojects/libdisplay-info/tool/gen-search-table.py default_extras_install.sh; do
      head -n 1 "$script" | grep "^#! *$NIX_STORE/"
    done
    mkdir -p "$out"
    cp src/Utils/DirHelpers.cpp src/Utils/Process.cpp "$out/"
    runHook postInstall
  '';
}
