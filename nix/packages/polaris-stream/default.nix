# polaris-stream — luxus/polaris fork (stream mode + runtime foundation) + phase patches.
# Base tracks github:luxus/polaris (not papi-ux tip) until the mode/runtime work is upstreamed.
# Packaging pattern adapted from Sunshine / nixpkgs GameStream hosts:
# LizardByte ffmpeg prebuilt, separate npm UI, optional CUDA, Linux udev paths.
# Linux-only (no Darwin focus). Browser Stream helper is prebuilt with buildGoModule.
{
  lib,
  stdenv,
  fetchFromGitHub,
  fetchgit,
  fetchzip,
  autoPatchelfHook,
  autoAddDriverRunpath,
  makeWrapper,
  buildNpmPackage,
  buildGoModule,
  cmake,
  avahi,
  libevdev,
  libpulseaudio,
  libxtst,
  libxrandr,
  libxi,
  libxfixes,
  libxdmcp,
  libx11,
  libxcb,
  openssl,
  libopus,
  boost,
  pkg-config,
  libdrm,
  wayland,
  wayland-scanner,
  libffi,
  libcap,
  libgbm,
  curl,
  pcre,
  pcre2,
  python3,
  libuuid,
  libselinux,
  libsepol,
  libthai,
  libdatrie,
  libxkbcommon,
  libepoxy,
  libva,
  libvdpau,
  libglvnd,
  numactl,
  amf-headers,
  svt-av1,
  shaderc,
  vulkan-headers,
  vulkan-loader,
  libappindicator,
  libnotify,
  pipewire,
  miniupnpc,
  nlohmann_json,
  config,
  coreutils,
  grim,
  labwc,
  wlr-randr,
  xwayland,
  xdpyinfo,
  xdg-utils,
  vpl-gpu-rt,
  cudaSupport ? config.cudaSupport,
  cudaPackages ? { },
  enableBrowserStream ? true,
  # Upstream-aligned phases (see polaris/README.md). Disable a phase when that
  # code lands on polaris main and drop the corresponding patch after rebase.
  # Phase 1: Portal/PipeWire + reliable SHM/MemFd (capture_transport=shm).
  enablePhase1Portal ? true,
  # Phase 2: LINEAR DmaBuf → Vulkan → CUDA (vulkan_cuda); sticky mmap_cuda fallback.
  enablePhase2VulkanCuda ? true,
  # Phase 4: HDR request/metadata/force alignment (not hybrid PQ+SDR).
  enablePhase4Hdr ? true,
  # Optional: ScreenCast on private bus (POLARIS_PORTAL_DBUS_ADDRESS) for KDE/KRDP coexistence.
  enablePortalPrivateBus ? true,
  # Back-compat alias for enablePhase2VulkanCuda.
  enablePortalDmabufLinear ? null,
  # Source tree. Flake always passes self (cleanSource). Optional absolute path
  # for out-of-tree overrides.
  polarisSrc,
}:
let
  stdenv' = if cudaSupport then cudaPackages.backendStdenv else stdenv;
  # Legacy flag wins only if explicitly set (non-null).
  phase2VulkanCuda =
    if enablePortalDmabufLinear == null then enablePhase2VulkanCuda else enablePortalDmabufLinear;

  buildDepsTag = "v2026.724.203728";
  ffmpegArch =
    {
      x86_64-linux = "Linux-x86_64";
      aarch64-linux = "Linux-aarch64";
    }
    .${stdenv.hostPlatform.system}
      or (throw "polaris-stream: unsupported system ${stdenv.hostPlatform.system} for prebuilt ffmpeg");
  ffmpegPrebuilt = fetchzip {
    url = "https://github.com/LizardByte/build-deps/releases/download/${buildDepsTag}/${ffmpegArch}-ffmpeg.tar.gz";
    hash =
      {
        x86_64-linux = "sha256-LCfUaUtO0Oc09JfUvWLxs2Ysu8Te0qafLcS3A0Qe67M=";
        aarch64-linux = "sha256-/WSS9V15rheNuX5I1jlbTKwqLhCy8Vew1ANVz9fBYOg=";
      }
      .${stdenv.hostPlatform.system};
  };

in
assert stdenv.hostPlatform.isLinux;
assert (!phase2VulkanCuda || enablePhase1Portal);
assert (!enablePortalPrivateBus || enablePhase1Portal);
stdenv'.mkDerivation (finalAttrs: {
  pname = "polaris-stream";
  # luxus/polaris fork. Phase1/2/4 + private bus + portal lock contract are on
  # feat/linux-stream-runtime tip (559aac4+). Historical topics/phase patches under
  # ../../polaris/ and ../../archived/polaris/ for older pins only.
  # After pushing feat/linux-stream-runtime (or master), update rev + hash.
  version = "0-unstable-2026-07-28";

  # fetchgit (not fetchFromGitHub): nested submodules (e.g. moonlight-common-c/enet)
  # must be present or cmake fails. Hash is recursive-submodule narHash.
  src = polarisSrc;

  # Phases 1/2/4 + private ScreenCast bus + SB-2 async portal destroy landed in
  # luxus/polaris >= 85e6733. Do not re-apply polaris-stream-topics-*.patch on this
  # pin (it would conflict). Retained on disk for 0118ace-era rebuilds only;
  # that file was also fixed for the nested-lock / negotiate-under-mutex bugs.
  # fix-cancel-owner-token.patch is upstream in luxus/polaris >= a364fb7 / 06b0925.
  # Phase flags below remain for step packages / documentation; they no longer
  # select patch files once the pin includes the phase code.
  patches = [ ];

  ui = buildNpmPackage {
    inherit (finalAttrs) src version;
    pname = "polaris-stream-ui";
    npmDepsHash = "sha256-zgDMvRbXZ5yQXlIZneO9Xuq2J4Xs2G4EgvewiP3QYts=";

    installPhase = ''
      runHook preInstall

      mkdir -p "$out"
      cp -a . "$out"/

      runHook postInstall
    '';
  };

  # WebTransport helper — built out-of-tree so CMake does not need network/Go.
  browserStreamHelper =
    if enableBrowserStream then
      buildGoModule {
        inherit (finalAttrs) src version;
        pname = "polaris-browser-stream-helper";
        modRoot = "browser_stream_helper";
        vendorHash = "sha256-MEQl1/E6vm+wGRwNWNaRiXaoZZ+bEzi8ItVXEexHujI=";
        subPackages = [ "." ];
      }
    else
      null;

  postPatch =
    # don't look for npm since we build webui separately
    ''
      substituteInPlace cmake/targets/common.cmake \
        --replace-fail 'find_program(NPM npm REQUIRED)' ""
    ''
    # use system boost instead of FetchContent
    + ''
      sed -i -E 's/set\(BOOST_VERSION "[^"]*"\)/set(BOOST_VERSION "${boost.version}")/' \
        cmake/dependencies/Boost_Polaris.cmake
      echo 'set(FETCH_CONTENT_BOOST_USED TRUE)' >> cmake/dependencies/Boost_Polaris.cmake
    ''
    # remove upstream dependency on systemd
    + ''
      substituteInPlace cmake/packaging/linux.cmake \
        --replace-fail 'find_package(Systemd)' ""

      substituteInPlace packaging/linux/polaris.service.in \
        --replace-fail '/bin/sleep' '${lib.getExe' coreutils "sleep"}'
    ''
    + lib.optionalString enableBrowserStream ''
      # Prebuilt Go helper; skip in-tree go build (needs network / home cache).
      substituteInPlace cmake/targets/common.cmake \
        --replace-fail 'find_program(GO_EXECUTABLE go REQUIRED)' 'set(GO_EXECUTABLE true)' \
        --replace-fail \
          'COMMAND "''${GO_EXECUTABLE}" ''${BROWSER_STREAM_HELPER_BUILD_ARGUMENTS} -o "''${BROWSER_STREAM_HELPER_OUTPUT}" .' \
          'COMMAND "''${CMAKE_COMMAND}" -E copy "${finalAttrs.browserStreamHelper}/bin/browser_stream_helper" "''${BROWSER_STREAM_HELPER_OUTPUT}"'
    '';

  nativeBuildInputs = [
    cmake
    pkg-config
    (python3.withPackages (ps: [
      ps.jinja2
      ps.setuptools
    ]))
    makeWrapper
    wayland-scanner
    shaderc
    autoPatchelfHook
  ]
  ++ lib.optionals cudaSupport [
    autoAddDriverRunpath
    cudaPackages.cuda_nvcc
    (lib.getDev cudaPackages.cuda_cudart)
  ];

  buildInputs = [
    boost
    curl
    miniupnpc
    nlohmann_json
    openssl
    libopus
    avahi
    libevdev
    libpulseaudio
    libx11
    libxcb
    libxfixes
    libxrandr
    libxtst
    libxi
    libdrm
    wayland
    libffi
    libcap
    pcre
    pcre2
    libuuid
    libselinux
    libsepol
    libthai
    libdatrie
    libxdmcp
    libxkbcommon
    libepoxy
    libva
    libvdpau
    numactl
    libgbm
    amf-headers
    svt-av1
    vulkan-loader
    pipewire
    libappindicator
    libnotify
  ]
  ++ lib.optionals cudaSupport [
    cudaPackages.cudatoolkit
    cudaPackages.cuda_cudart
    vulkan-headers
  ];

  runtimeDependencies = [
    avahi
    libgbm
    libxrandr
    libxcb
    libglvnd
    grim
    labwc
    wlr-randr
    xwayland
    xdpyinfo
    vpl-gpu-rt
  ];

  cmakeFlags = [
    "-Wno-dev"
    (lib.cmakeBool "BOOST_USE_STATIC" false)
    (lib.cmakeBool "BUILD_DOCS" false)
    (lib.cmakeBool "BUILD_TESTS" false)
    # Avoid impure -march=native if upstream ever enables it for "native arch".
    (lib.cmakeBool "POLARIS_ENABLE_NATIVE_ARCH" false)
    (lib.cmakeBool "POLARIS_ENABLE_BROWSER_STREAM" enableBrowserStream)
    (lib.cmakeFeature "POLARIS_PUBLISHER_NAME" "polaris")
    (lib.cmakeFeature "POLARIS_PUBLISHER_WEBSITE" "https://github.com/luxus/polaris")
    (lib.cmakeFeature "POLARIS_PUBLISHER_ISSUE_URL" "https://github.com/papi-ux/polaris/issues")
    (lib.cmakeFeature "FFMPEG_PREPARED_BINARIES" "${ffmpegPrebuilt}")
    (lib.cmakeBool "POLARIS_DOWNLOAD_PREPARED_FFMPEG" false)
    (lib.cmakeBool "GLAD_SKIP_PIP_INSTALL" true)
    (lib.cmakeBool "UDEV_FOUND" true)
    (lib.cmakeBool "SYSTEMD_FOUND" true)
    (lib.cmakeFeature "UDEV_RULES_INSTALL_DIR" "lib/udev/rules.d")
    (lib.cmakeFeature "SYSTEMD_USER_UNIT_INSTALL_DIR" "lib/systemd/user")
    (lib.cmakeFeature "SYSTEMD_MODULES_LOAD_DIR" "lib/modules-load.d")
    (lib.cmakeFeature "POLARIS_EXECUTABLE_PATH" "${placeholder "out"}/bin/polaris")
  ]
  ++ lib.optionals (!cudaSupport) [
    (lib.cmakeBool "POLARIS_ENABLE_CUDA" false)
    (lib.cmakeBool "POLARIS_ALLOW_CUDA_DISABLED_ON_NVIDIA" true)
  ];

  env = {
    BUILD_VERSION = "0-unstable-2026-07-28";
    BRANCH = "feat/linux-stream-runtime";
    # Matches src.rev (EasyEffects capture when host default is a processing sink).
    COMMIT = "ad0ed6bf0e6a73457c9ce0b3dd4762fb350b7479";
  };

  # cmake runs in $source/build; stamp files so web-ui / browser-stream targets
  # do not invoke npm/go during the main build.
  postConfigure = ''
    mkdir -p ../node_modules CMakeFiles
    touch ../node_modules/.polaris-web-ui-npm.stamp
    touch CMakeFiles/web-ui-build.stamp
  '';

  buildFlags = [
    "polaris"
  ];

  preInstall = ''
    rm -rf assets/web
    mkdir -p assets/web
    cp -r ${finalAttrs.ui}/build/assets/web/. assets/web/
  '';

  installPhase = ''
    runHook preInstall

    cmake --install .

    # --setup-host copies these from assets/; expose standard paths for NixOS udev.
    mkdir -p "$out/lib/udev/rules.d" "$out/lib/modules-load.d"
    cp -r "$out/assets/udev/rules.d/." "$out/lib/udev/rules.d/"
    cp -r "$out/assets/modules-load.d/." "$out/lib/modules-load.d/"

    runHook postInstall
  '';

  # Headless Stream shells out to labwc/grim/wlr-randr; tray "Open settings"
  # needs xdg-open (user unit PATH is minimal).
  postFixup = ''
    wrapProgram $out/bin/polaris \
      --prefix PATH : ${
        lib.makeBinPath [
          grim
          labwc
          wlr-randr
          xwayland
          xdpyinfo
          xdg-utils
        ]
      } \
      ${lib.optionalString cudaSupport "--prefix LD_LIBRARY_PATH : ${
        lib.makeLibraryPath [ vulkan-loader ]
      }"}
  '';

  meta = {
    description = "Self-hosted game stream host for Moonlight";
    homepage = "https://github.com/papi-ux/polaris";
    license = lib.licenses.gpl3Only;
    mainProgram = "polaris";
    maintainers = [ ];
    platforms = lib.platforms.linux;
  };
})
