# home-manager per-user module — same options/defaults as hjem.
# Not tested under HM here; shares options.nix + session-lib.nix with hjem.
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
  ready =
    if cfg.desktopUserReadyTarget != null then cfg.desktopUserReadyTarget else cfg.desktopUserTarget;
in
{
  options.services.polaris = import ./options.nix { inherit lib pkgs; };

  config = lib.mkIf cfg.enable {
    home.packages = session.packages;

    systemd.user.services.polaris-gamescope-idle = {
      Unit = {
        Description = "Idle gamescope for Polaris";
        After = [
          cfg.desktopUserTarget
          ready
        ];
        PartOf = [ cfg.desktopUserTarget ];
      };
      Service = {
        Type = "simple";
        ExecStart = lib.getExe session.idleApp;
        Restart = "on-failure";
        RestartSec = "2s";
        Environment = session.envToList session.baseEnvironment;
        UnsetEnvironment = [ "WAYLAND_DISPLAY" ];
      };
      Install.WantedBy = [ cfg.desktopUserTarget ];
    };

    systemd.user.services.polaris-portal-dbus = {
      Unit = {
        Description = "Private D-Bus session bus for Polaris ScreenCast portal";
        After = [
          cfg.desktopUserTarget
          ready
        ];
        PartOf = [ cfg.desktopUserTarget ];
      };
      Service = {
        Type = "simple";
        RuntimeDirectory = "polaris-portal";
        RuntimeDirectoryMode = "0700";
        ExecStart = session.portalBusExec;
        Restart = "on-failure";
        RestartSec = "1s";
      };
      Install.WantedBy = [ cfg.desktopUserTarget ];
    };

    systemd.user.services.polaris-portal-gamescope = {
      Unit = {
        Description = "Gamescope ScreenCast backend for Polaris private portal";
        After = [
          cfg.desktopUserTarget
          ready
          "polaris-gamescope-idle.service"
          "polaris-portal-dbus.service"
        ];
        Wants = [ "polaris-gamescope-idle.service" ];
        Requires = [ "polaris-portal-dbus.service" ];
        PartOf = [ cfg.desktopUserTarget ];
      };
      Service = {
        Type = "simple";
        ExecStart = session.portalBackendExec;
        Restart = "on-failure";
        RestartSec = "1s";
        Environment = session.envToList session.portalEnvironment;
      };
      Install.WantedBy = [ cfg.desktopUserTarget ];
    };

    systemd.user.services.polaris-portal = {
      Unit = {
        Description = "Private XDG desktop portal for Polaris";
        After = [
          cfg.desktopUserTarget
          ready
          "polaris-portal-dbus.service"
          "polaris-portal-gamescope.service"
        ];
        Wants = [ "polaris-portal-gamescope.service" ];
        Requires = [ "polaris-portal-dbus.service" ];
        PartOf = [ cfg.desktopUserTarget ];
      };
      Service = {
        Type = "simple";
        ExecStart = session.portalFrontendExec;
        # always: frontend can be cleanly stopped while polaris stays up; stream
        # needs org.freedesktop.portal.Desktop on the private bus.
        Restart = "always";
        RestartSec = "1s";
        Environment = session.envToList session.portalEnvironment;
      };
      Install.WantedBy = [ cfg.desktopUserTarget ];
    };

    systemd.user.services.polaris = {
      Unit = {
        Description = "Polaris game stream host for Moonlight";
        After = [
          cfg.desktopUserTarget
          ready
          "polaris-gamescope-idle.service"
          "polaris-portal-dbus.service"
          "polaris-portal-gamescope.service"
          "polaris-portal.service"
        ];
        Wants = [
          "polaris-gamescope-idle.service"
          "polaris-portal-dbus.service"
          "polaris-portal-gamescope.service"
          "polaris-portal.service"
        ];
        Requires = [
          "polaris-portal-dbus.service"
          "polaris-portal-gamescope.service"
          "polaris-portal.service"
        ];
        PartOf = [ cfg.desktopUserTarget ];
      };
      Service = {
        Type = "simple";
        ExecStartPre = "${session.waitPortal}";
        ExecStart = "${session.polarisStart}";
        Restart = "on-failure";
        RestartSec = "5s";
        LimitRTPRIO = 95;
        LimitNICE = -10;
        Environment = session.envToList session.polarisServiceEnvironment;
        UnsetEnvironment = [ "WAYLAND_DISPLAY" ];
      };
      Install.WantedBy = [ cfg.desktopUserTarget ];
    };

    home.activation.polarisConfSeed = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      confdir="$HOME/.config/polaris"
      mkdir -p "$confdir"
      if [ ! -f "$confdir/polaris.conf" ]; then
        cp ${session.polarisConfSeed} "$confdir/polaris.conf"
        chmod 600 "$confdir/polaris.conf"
      fi
    '';
  };
}
