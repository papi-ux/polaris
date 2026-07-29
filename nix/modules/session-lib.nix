# Shared builders for polaris user session stack.
# GPU selection: polaris.conf adapter_name (web UI) — no hybrid pin rewrite.
{
  lib,
  pkgs,
  cfg,
}:
let
  gamescope = cfg.packageGamescope;
  portal = cfg.packagePortal;
  polarisPkg = cfg.package;
  target = cfg.desktopUserTarget;
  ready = if cfg.desktopUserReadyTarget != null then cfg.desktopUserReadyTarget else target;
  privatePortalAddress = "unix:path=%t/polaris-portal/bus";
  portalEnvironment = {
    DBUS_SESSION_BUS_ADDRESS = privatePortalAddress;
    WAYLAND_DISPLAY = "gamescope-0";
    XDG_CURRENT_DESKTOP = "gamescope";
    XDG_DESKTOP_PORTAL_DIR = "${portal}/share/xdg-desktop-portal/portals";
  };
  portalBusExec = "${pkgs.dbus}/bin/dbus-daemon --session --nofork --nopidfile --address=${privatePortalAddress}";
  portalBackendExec = "${portal}/libexec/xdg-desktop-portal-gamescope";
  portalFrontendExec = "${pkgs.xdg-desktop-portal}/libexec/xdg-desktop-portal -r";

  # Unit Environment= lines from options (preferVk + free-form attrs).
  baseEnvironment = {
    POLARIS_HDR_WIDTH = toString cfg.width;
    POLARIS_HDR_HEIGHT = toString cfg.height;
    POLARIS_HDR_REFRESH = toString cfg.refresh;
  }
  // lib.optionalAttrs (cfg.preferVkDevice != null) {
    POLARIS_GAMESCOPE_PREFER_VK = cfg.preferVkDevice;
  }
  // (cfg.environment or { });

  # Host WAYLAND_DISPLAY must not be passed: polaris binds probes to KWin and
  # gamescope lacks xdg-output for wlgrab. Capture uses GAMESCOPE_WAYLAND_DISPLAY.
  polarisServiceEnvironment = baseEnvironment // {
    GAMESCOPE_WAYLAND_DISPLAY = "gamescope-0";
    XDG_CURRENT_DESKTOP = "gamescope";
    DISPLAY = ":0";
  };

  envToUnitLines =
    env: lib.concatStringsSep "\n" (lib.mapAttrsToList (k: v: "Environment=${k}=${v}") env);

  envToList = env: lib.mapAttrsToList (k: v: "${k}=${v}") env;

  polarisConfSeed =
    if cfg.streamMode == "gamescope_stream" then
      pkgs.writeText "polaris.conf" ''
        headless_mode = enabled
        linux_use_cage_compositor = enabled
        linux_prefer_gpu_native_capture = disabled
        linux_stream_mode = gamescope_stream
        capture = portal
        encoder = nvenc
        hevc_mode = 3
        av1_mode = 0
        stream_audio = enabled
        enable_pairing = enabled
        enable_discovery = enabled
        max_sessions = 2
      ''
    else
      pkgs.writeText "polaris.conf" ''
        headless_mode = enabled
        linux_use_cage_compositor = enabled
        linux_prefer_gpu_native_capture = disabled
        linux_stream_mode = labwc
        encoder = nvenc
        stream_audio = enabled
        enable_pairing = enabled
        enable_discovery = enabled
        max_sessions = 2
      '';

  idleApp = pkgs.writeShellApplication {
    name = "polaris-gamescope-idle";
    runtimeInputs = [
      pkgs.bash
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.gnused
      pkgs.util-linux
      gamescope # cfg.packageGamescope (gamescope-polaris), not stock pkgs.gamescope
    ];
    text = ''
      export POLARIS_HDR_WIDTH="''${POLARIS_HDR_WIDTH:-${toString cfg.width}}"
      export POLARIS_HDR_HEIGHT="''${POLARIS_HDR_HEIGHT:-${toString cfg.height}}"
      export POLARIS_HDR_REFRESH="''${POLARIS_HDR_REFRESH:-${toString cfg.refresh}}"
      export POLARIS_SDR_GAMUT_WIDENESS="''${POLARIS_SDR_GAMUT_WIDENESS:-${toString cfg.sdrGamutWideness}}"
      export POLARIS_SDR_CONTENT_NITS="''${POLARIS_SDR_CONTENT_NITS:-${toString cfg.sdrContentNits}}"
      ${builtins.readFile ./polaris-gamescope-runtime-lib.sh}
      ${builtins.readFile ../../scripts/install/lib/polaris-gamescope-idle.sh}
    '';
  };

  sessionBin = pkgs.writeShellApplication {
    name = "polaris-gamescope-session";
    runtimeInputs = [
      pkgs.coreutils
      gamescope # cfg.packageGamescope
      pkgs.gnugrep
      pkgs.gnused
      pkgs.procps
      pkgs.pulseaudio
      pkgs.systemd
      pkgs.util-linux
      pkgs.wireplumber
    ];
    text = ''
      export POLARIS_GAMESCOPE_BIN=${lib.getExe gamescope}
      export POLARIS_SESSION_PATH="$PATH"
      export POLARIS_HDR_WIDTH="''${POLARIS_HDR_WIDTH:-${toString cfg.width}}"
      export POLARIS_HDR_HEIGHT="''${POLARIS_HDR_HEIGHT:-${toString cfg.height}}"
      export POLARIS_HDR_REFRESH="''${POLARIS_HDR_REFRESH:-${toString cfg.refresh}}"
      ${builtins.readFile ./polaris-gamescope-runtime-lib.sh}
      ${builtins.readFile ./polaris-gamescope-session.sh}
    '';
  };

  polarisStart = pkgs.writeShellScript "polaris-start" ''
    set -euo pipefail
    confdir="''${XDG_CONFIG_HOME:-$HOME/.config}/polaris"
    mkdir -p "$confdir"
    if [ ! -f "$confdir/polaris.conf" ]; then
      cp ${polarisConfSeed} "$confdir/polaris.conf"
      chmod 600 "$confdir/polaris.conf"
    fi

    # The managed service is coupled to its private portal generation. Recheck
    # both names immediately before exec so readiness cannot silently fall back
    # to an unrelated host-session portal.
    rt="''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    bus_path="$rt/polaris-portal/bus"
    private_address="unix:path=$bus_path"
    if [ ! -S "$bus_path" ] \
      || ! ${pkgs.systemd}/bin/busctl --address="$private_address" --no-pager \
        status org.freedesktop.portal.Desktop >/dev/null 2>&1 \
      || ! ${pkgs.systemd}/bin/busctl --address="$private_address" --no-pager \
        status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1; then
      echo "polaris: required private ScreenCast portal disappeared before startup" >&2
      exit 1
    fi
    export POLARIS_PORTAL_DBUS_ADDRESS="$private_address"

    exec ${lib.getExe polarisPkg}
  '';

  waitPortal = pkgs.writeShellScript "polaris-wait-private-screencast" ''
    set -euo pipefail
    rt="''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    export POLARIS_FLOCK_BIN=${lib.getExe' pkgs.util-linux "flock"}
    ${builtins.readFile ./polaris-gamescope-runtime-lib.sh}

    # Nested stop can leave runtime-masked idle / no gamescope-0.
    if [ -f "$rt/polaris-gamescope-wsi-nested" ] || [ ! -S "$rt/gamescope-0" ]; then
      echo "polaris: recover idle gamescope-0 (nested leftover or missing socket)" >&2
      if polaris_validate_marker "$rt/polaris-gamescope.pid" nested; then
        polaris_stop_marked_gamescope "$rt/polaris-gamescope.pid" nested "$rt" || true
      fi
      rm -f "$rt/polaris-gamescope-wsi-nested" "$rt/polaris-gamescope-appid" \
        "$rt/polaris-gamescope-audio-sink" || true
      polaris_unmask_idle_unit_runtime
      if [ ! -S "$rt/gamescope-0" ]; then
        ${pkgs.systemd}/bin/systemctl --user restart polaris-gamescope-idle.service 2>/dev/null \
          || ${pkgs.systemd}/bin/systemctl --user start polaris-gamescope-idle.service 2>/dev/null || true
      fi
    fi

    bus_path="$rt/polaris-portal/bus"

    deadline=$((SECONDS + 60))
    while [ ! -S "$rt/gamescope-0" ]; do
      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "polaris: timed out waiting for gamescope-0" >&2
        exit 1
      fi
      sleep 0.2
    done

    bus_deadline=$((SECONDS + 10))
    while [ ! -S "$bus_path" ] && [ "$SECONDS" -lt "$bus_deadline" ]; do
      sleep 0.1
    done
    if [ ! -S "$bus_path" ]; then
      echo "polaris: required private portal bus did not appear" >&2
      exit 1
    fi

    private_address="unix:path=$bus_path"
    deadline=$((SECONDS + 45))
    while true; do
      modes="$(${pkgs.systemd}/bin/busctl --address="$private_address" get-property \
        org.freedesktop.impl.portal.desktop.gamescope \
        /org/freedesktop/portal/desktop \
        org.freedesktop.impl.portal.ScreenCast AvailableCursorModes 2>/dev/null \
        | ${pkgs.gawk}/bin/awk '{print $2}' || true)"
      if [ -n "''${modes:-}" ] && [ "''${modes}" != "0" ]; then
        echo "polaris: private ScreenCast ready (gamescope-0 + portal, cursor_modes=$modes)" >&2
        exit 0
      fi
      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "polaris: required private ScreenCast portal did not become ready" >&2
        exit 1
      fi
      sleep 0.25
    done
  '';

  mkUnit =
    {
      description,
      after ? [ ],
      wants ? [ ],
      requires ? [ ],
      partOf ? [ target ],
      wantedBy ? [ target ],
      serviceConfig,
    }:
    let
      after' = lib.concatStringsSep " " (lib.unique (after ++ [ ready ]));
      wants' = lib.concatStringsSep " " wants;
      requires' = lib.concatStringsSep " " requires;
      partOf' = lib.concatStringsSep " " partOf;
      wantedBy' = lib.concatStringsSep " " wantedBy;
    in
    ''
      [Unit]
      Description=${description}
      After=${after'}
      ${lib.optionalString (wants != [ ]) "Wants=${wants'}"}
      ${lib.optionalString (requires != [ ]) "Requires=${requires'}"}
      PartOf=${partOf'}

      [Service]
      ${serviceConfig}

      [Install]
      WantedBy=${wantedBy'}
    '';

  userUnitTexts = {
    "polaris-gamescope-idle.service" = mkUnit {
      description = "Idle gamescope for Polaris (portal capture target)";
      after = [ target ];
      serviceConfig = ''
        Type=simple
        ExecStart=${lib.getExe idleApp}
        # on-failure: gamescope ABRT ends the wrapper with exit 134; on-abnormal
        # ignores that and leaves idle permanently failed (orphan sockets stick).
        # Nested WSI masks via user.control (not plain mask --runtime) so
        # portal-gamescope Wants= cannot respawn idle under ~/.config units.
        Restart=on-failure
        RestartSec=5s
        TimeoutStopSec=10s
        ${envToUnitLines baseEnvironment}
        PassEnvironment=XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
        UnsetEnvironment=WAYLAND_DISPLAY
      '';
    };
    "polaris-portal-dbus.service" = mkUnit {
      description = "Private D-Bus session bus for Polaris ScreenCast portal";
      after = [ target ];
      serviceConfig = ''
        Type=simple
        RuntimeDirectory=polaris-portal
        RuntimeDirectoryMode=0700
        ExecStart=${portalBusExec}
        Restart=on-failure
        RestartSec=1s
      '';
    };
    "polaris-portal-gamescope.service" = mkUnit {
      description = "Gamescope ScreenCast backend for Polaris private portal";
      after = [
        "polaris-gamescope-idle.service"
        "polaris-portal-dbus.service"
      ];
      wants = [ "polaris-gamescope-idle.service" ];
      requires = [ "polaris-portal-dbus.service" ];
      serviceConfig = ''
        Type=simple
        ExecStart=${portalBackendExec}
        Restart=on-failure
        RestartSec=1s
        ${envToUnitLines portalEnvironment}
      '';
    };
    "polaris-portal.service" = mkUnit {
      description = "Private XDG desktop portal for Polaris";
      after = [
        "polaris-portal-dbus.service"
        "polaris-portal-gamescope.service"
      ];
      wants = [ "polaris-portal-gamescope.service" ];
      requires = [ "polaris-portal-dbus.service" ];
      serviceConfig = ''
        Type=simple
        ExecStart=${portalFrontendExec}
        # always: frontend can be cleanly stopped while polaris stays up; stream
        # needs org.freedesktop.portal.Desktop on the private bus.
        Restart=always
        RestartSec=1s
        ${envToUnitLines portalEnvironment}
      '';
    };
    "polaris.service" = mkUnit {
      description = "Polaris game stream host for Moonlight";
      after = [
        target
        "polaris-gamescope-idle.service"
        "polaris-portal-dbus.service"
        "polaris-portal-gamescope.service"
        "polaris-portal.service"
      ];
      wants = [
        "polaris-gamescope-idle.service"
        "polaris-portal.service"
      ];
      requires = [
        "polaris-portal-dbus.service"
        "polaris-portal-gamescope.service"
        "polaris-portal.service"
      ];
      serviceConfig = ''
        Type=simple
        ExecStartPre=${waitPortal}
        ExecStart=${polarisStart}
        Restart=on-failure
        RestartSec=5s
        LimitRTPRIO=95
        LimitNICE=-10
        ${envToUnitLines polarisServiceEnvironment}
        Environment=PATH=${
          lib.makeBinPath [
            pkgs.steam
            pkgs.bashInteractive
            pkgs.bubblewrap
            pkgs.util-linux
            pkgs.coreutils
            sessionBin
            gamescope
          ]
        }:%h/.local/bin:/run/current-system/sw/bin
        # Do not PassEnvironment WAYLAND_DISPLAY — host KWin would override gamescope path.
        PassEnvironment=DISPLAY XDG_SESSION_TYPE XDG_SESSION_ID XAUTHORITY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
        UnsetEnvironment=WAYLAND_DISPLAY
      '';
    };
  };

  # Store paths with correct basenames so .wants/ links work under hjem
  # (relative symlinks break atomic activation — both sides must be .source).
  writeUnit =
    unitName: text:
    let
      dir = pkgs.writeTextDir unitName text;
    in
    "${dir}/${unitName}";

  unitPaths = lib.mapAttrs writeUnit userUnitTexts;

  # hjem files: unit + compositor.wants/ pull-in (Install WantedBy alone is not enough
  # without systemctl enable; .wants/ is the declarative equivalent).
  userUnitFiles =
    (lib.mapAttrs' (
      name: path: lib.nameValuePair ".config/systemd/user/${name}" { source = path; }
    ) unitPaths)
    // (lib.mapAttrs' (
      name: path:
      lib.nameValuePair ".config/systemd/user/${target}.wants/${name}" { source = path; }
    ) unitPaths);

in
{
  inherit
    idleApp
    sessionBin
    polarisStart
    waitPortal
    polarisConfSeed
    gamescope
    portal
    polarisPkg
    target
    ready
    privatePortalAddress
    portalEnvironment
    portalBusExec
    portalBackendExec
    portalFrontendExec
    userUnitTexts
    unitPaths
    userUnitFiles
    ;

  packages = [
    polarisPkg
    gamescope
    portal
    pkgs.dbus
    pkgs.xdg-desktop-portal
    idleApp
    sessionBin
    pkgs.steam
    pkgs.bubblewrap
    pkgs.util-linux
    pkgs.wlr-randr
  ];

  # For home-manager systemd.user.services.*.Service.Environment
  inherit baseEnvironment polarisServiceEnvironment envToList;
}
