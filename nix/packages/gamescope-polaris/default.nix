# gamescope for Polaris (HDR capture stack (polaris#152).
# enableWsi=true always: layer built; attach-only has been flaky — keep nested path available.
#
# Tracks ValveSoftware/gamescope master (not only the nixpkgs tag) for compositor
# fixes. Re-check patches + meson flags after each bump.
# Drop checklist: nix/patches/gamescope/README.md (grep POLARIS-UPSTREAM-REMOVE).
{
  gamescope,
  fetchFromGitHub,
  lib,
}:

let
  # Master tip 2026-08-01.
  gamescopeRev = "ff6b924fd0634a51d0fb3755c56c01dca1daadc1";

  # Master switched glm/stb from system headers to meson wrap-git subprojects.
  # Vendoring keeps wrap_mode=nodownload happy in the nix sandbox.
  glmSrc = fetchFromGitHub {
    owner = "g-truc";
    repo = "glm";
    rev = "0af55ccecd98d4e5a8d1fad7de25ba429d60e863";
    hash = "sha256-GnGyzNRpzuguc3yYbEFtYLvG+KiCtRAktiN+NvbOICE=";
  };
  stbSrc = fetchFromGitHub {
    owner = "nothings";
    repo = "stb";
    rev = "5736b15f7ea0ffb08dd38af21067c314d6a3aae9";
    hash = "sha256-s2ASdlT3bBNrqvwfhhN6skjbmyEnUgvNOrvhgUSRj98=";
  };
in
(gamescope.override { enableWsi = true; }).overrideAttrs (old: {
  pname = "gamescope-polaris";
  version = "0-unstable-2026-08-01";

  src = fetchFromGitHub {
    owner = "ValveSoftware";
    repo = "gamescope";
    rev = gamescopeRev;
    fetchSubmodules = true;
    hash = "sha256-WkaTWBZuUR/EPMb7btuqIgK2M8HivEYBWLwrMDsxVwY=";
  };

  # Keep only nixpkgs packaging patches that still apply on master.
  # The two pending upstream fetchpatches on 3.16.24 are already in master.
  patches =
    (builtins.filter (
      p:
      let
        s = toString p;
      in
      lib.hasInfix "shaders-path" s || lib.hasInfix "gamescopereaper" s
    ) (old.patches or [ ]))
    ++ [
      # ValveSoftware/gamescope#2270 — SPA xBGR_210LE + HDR metadata + renegotiate.
      # DROP when #2270 merges. Grep: POLARIS-UPSTREAM-REMOVE.*2270
      ../../patches/gamescope/01-pipewire-xbgr-210le-2270.patch
      ../../patches/gamescope/02-headless-hdr-colorimetry.patch
      ../../patches/gamescope/03-pipewire-prefer-dmabuf.patch
      # Screenshot SDR/HDR LUTs on PW paint (locked to EOTF).
      ../../patches/gamescope/04-pipewire-color-mgmt.patch
      # ValveSoftware/gamescope#2217: headless prefers discrete GPU if unpinned.
      # DROP when #2217 merges. Grep: POLARIS-UPSTREAM-REMOVE.*2217
      ../../patches/gamescope/06-prefer-discrete-gpu-2217.patch
      # Companion to #2270: paint_pipewire EOTF_PQ + SDR-on-HDR screenshot defaults.
      # DROP when upstream paint matches SPA HDR metadata. Grep: companion to.*2270
      ../../patches/gamescope/07-paint-pipewire-eotf-pq.patch
    ];

  # Master dropped glm_include_dir / stb_include_dir meson options.
  mesonFlags = [
    (lib.mesonBool "enable_gamescope" true)
    (lib.mesonBool "enable_gamescope_wsi_layer" true)
    (lib.mesonBool "enable_tests" false)
  ];

  # Materialize glm/stb wrap-git deps from nix store (sandbox has no network).
  postPatch =
    (old.postPatch or "")
    + ''
      rm -rf subprojects/glm subprojects/stb
      cp -a ${glmSrc} subprojects/glm
      cp -a ${stbSrc} subprojects/stb
      chmod -R u+w subprojects/glm subprojects/stb
      cp -f subprojects/packagefiles/glm/meson.build subprojects/glm/meson.build
      cp -f subprojects/packagefiles/stb/meson.build subprojects/stb/meson.build
    '';

  meta = old.meta // {
    description = "${
      old.meta.description or "gamescope"
    } (polaris HDR PW stack; #2270/#2217; master ${lib.substring 0 7 gamescopeRev})";
  };
})
