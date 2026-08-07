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
      # HDR capture stack (format negotiation + optional cursor + stamp).
      # DROP 10/11 when equivalent upstream lands; stamp is Polaris-only.
      ../../patches/gamescope/10-pipewire-offer-10-bit-BT2020-PQ.patch
      ../../patches/gamescope/11-pipewire-composite-cursor.patch
      ../../patches/gamescope/12-polaris-stamp-version-polhdrN.patch
      # Polaris-only keepers until proven redundant with 10:
      ../../patches/gamescope/02-headless-hdr-colorimetry.patch
      ../../patches/gamescope/03-pipewire-prefer-dmabuf.patch
      # ValveSoftware/gamescope#2217: headless prefers discrete GPU if unpinned.
      ../../patches/gamescope/06-prefer-discrete-gpu-2217.patch
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

  # Capability stamp from patch 12 — fail the build if patches did not apply.
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    # wrapProgram may hide --version on the outer wrapper; try both.
    # Ignore --version exit status: only the version string matters, and pipefail
    # would fail the check if gamescope exits non-zero after printing it.
    ver_out="$("$out/bin/gamescope" --version 2>&1 || true)"
    if ! printf '%s\n' "$ver_out" | grep -q '+polhdr'; then
      if [ -x "$out/bin/.gamescope-wrapped" ]; then
        ver_out="$("$out/bin/.gamescope-wrapped" --version 2>&1 || true)"
      fi
      if ! printf '%s\n' "$ver_out" | grep -q '+polhdr'; then
        echo "gamescope-polaris: +polhdr marker missing from --version" >&2
        printf '%s\n' "$ver_out" || true
        exit 1
      fi
    fi
    runHook postInstallCheck
  '';

  meta = old.meta // {
    description = "${
      old.meta.description or "gamescope"
    } (polaris HDR PW +polhdr2; #2217; master ${lib.substring 0 7 gamescopeRev})";
  };
})
