# NixOS: host plumbing (packages, udev, firewall, groups).
# Per-user units: hjemModules / homeModules (same option defaults).
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.polaris;
in
{
  options.services.polaris = import ./options.nix { inherit lib pkgs; };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package
      cfg.packageGamescope
      cfg.packagePortal
    ];

    hardware.uinput.enable = true;
    boot.kernelModules = [ "uhid" ];
    services.udev.packages = [ cfg.package ];

    services.avahi = {
      enable = lib.mkDefault true;
      publish = {
        enable = lib.mkDefault true;
        userServices = lib.mkDefault true;
      };
    };

    networking.firewall.allowedTCPPorts = [
      47984
      47989
      47990
      48010
    ];
    networking.firewall.allowedUDPPorts = [
      47998
      47999
      48000
      48010
    ];

    users.users = lib.mkMerge (
      map (u: {
        ${u}.extraGroups = [
          "audio"
          "uinput"
          "video"
          "render"
          "input"
        ];
      }) cfg.users
    );
  };
}
