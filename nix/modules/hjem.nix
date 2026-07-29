# Hjem per-user module — packages, conf seed, systemd user units.
# Same options as homeModules.polaris (import options.nix).
#
# Activation: hjem writes unit files + ${desktopUserTarget}.wants/ links so the
# compositor target pulls polaris in (no separate systemctl enable required).
# Run `polaris-hjem-activate` after first switch if units were already loaded
# (daemon-reload + optional restart).
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.polaris;
  session = import ./session-lib.nix {
    inherit lib pkgs;
    inherit cfg;
  };

  activate = pkgs.writeShellApplication {
    name = "polaris-hjem-activate";
    runtimeInputs = [
      pkgs.systemd
      pkgs.coreutils
    ];
    text = ''
      set -euo pipefail
      confdir="''${XDG_CONFIG_HOME:-$HOME/.config}/polaris"
      mkdir -p "$confdir"
      if [ ! -f "$confdir/polaris.conf" ]; then
        cp ${session.polarisConfSeed} "$confdir/polaris.conf"
        chmod 600 "$confdir/polaris.conf"
        echo "polaris-hjem-activate: seeded $confdir/polaris.conf"
      fi
      systemctl --user daemon-reload
      # .wants/ from hjem should already pull units; enable is idempotent fallback.
      systemctl --user enable polaris-gamescope-idle.service polaris.service 2>/dev/null || true
      if systemctl --user is-active --quiet "${cfg.desktopUserTarget}" 2>/dev/null \
        || systemctl --user is-active --quiet graphical-session.target 2>/dev/null; then
        systemctl --user restart polaris-gamescope-idle.service \
          || systemctl --user start polaris-gamescope-idle.service || true
        systemctl --user restart polaris.service \
          || systemctl --user start polaris.service || true
        echo "polaris-hjem-activate: restarted polaris units"
      else
        echo "polaris-hjem-activate: units linked; start after login (${cfg.desktopUserTarget})"
      fi
      systemctl --user --no-pager --full status polaris.service polaris-gamescope-idle.service || true
    '';
  };
in
{
  options.services.polaris = import ./options.nix { inherit lib pkgs; };

  config = lib.mkIf cfg.enable {
    packages = session.packages ++ [
      pkgs.systemd
      activate
    ];

    files = session.userUnitFiles;
  };
}
