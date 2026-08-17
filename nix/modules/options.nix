# Single source of defaults for services.polaris (NixOS + hjem + home-manager).
# All three modules import this so HM/hjem stay identical without copy-paste.
{ lib, pkgs }:
{
  enable = lib.mkEnableOption "Polaris game stream host";

  package = lib.mkOption {
    type = lib.types.package;
    default = pkgs.polaris-stream;
    defaultText = lib.literalExpression "pkgs.polaris-stream";
    description = "Polaris package (from this flake overlay).";
  };

  packageGamescope = lib.mkOption {
    type = lib.types.package;
    default = pkgs.gamescope-polaris or pkgs.gamescope-hdr;
    defaultText = lib.literalExpression "pkgs.gamescope-polaris";
    description = "Patched gamescope for portal / nested HDR.";
  };

  packagePortal = lib.mkOption {
    type = lib.types.package;
    default = pkgs.xdg-desktop-portal-gamescope;
    defaultText = lib.literalExpression "pkgs.xdg-desktop-portal-gamescope";
    description = "xdg-desktop-portal-gamescope for private ScreenCast.";
  };

  streamMode = lib.mkOption {
    type = lib.types.enum [
      "gamescope_stream"
      "labwc"
    ];
    default = "gamescope_stream";
    description = ''
      Default path seeded into ~/.config/polaris/polaris.conf when missing
      (web UI owns later edits). Encode GPU: adapter_name in the web UI.
    '';
  };

  width = lib.mkOption {
    type = lib.types.ints.positive;
    default = 3840;
    description = "Headless gamescope width → POLARIS_HDR_WIDTH.";
  };
  height = lib.mkOption {
    type = lib.types.ints.positive;
    default = 2160;
    description = "Headless gamescope height → POLARIS_HDR_HEIGHT.";
  };
  refresh = lib.mkOption {
    type = lib.types.ints.positive;
    default = 120;
    description = "Headless gamescope refresh → POLARIS_HDR_REFRESH.";
  };

  sdrContentNits = lib.mkOption {
    type = lib.types.ints.positive;
    default = 203;
    description = "gamescope --hdr-sdr-content-nits when client requests HDR.";
  };
  sdrGamutWideness = lib.mkOption {
    type = lib.types.float;
    default = 0.0;
    description = "gamescope --sdr-gamut-wideness when client requests HDR.";
  };

  desktopUserTarget = lib.mkOption {
    type = lib.types.str;
    default = "graphical-session.target";
    example = "graphical-session.target";
    description = "systemd user target (WantedBy/PartOf) for polaris units.";
  };
  desktopUserReadyTarget = lib.mkOption {
    type = lib.types.nullOr lib.types.str;
    default = null;
    example = "graphical-session-pre.target";
    description = "Optional After= ready target (null → same as desktopUserTarget).";
  };

  preferVkDevice = lib.mkOption {
    type = lib.types.nullOr lib.types.str;
    default = null;
    example = "10de:2684";
    description = ''
      gamescope `--prefer-vk-device` (PCI vendor:device, no 0x).
      Default null = OS device order. Does not change web UI adapter_name.
    '';
  };

  environment = lib.mkOption {
    type = lib.types.attrsOf lib.types.str;
    default = { };
    example = {
      POLARIS_STEAM_ON_GAMESCOPE = "1";
    };
    description = ''
      Extra Environment= on polaris + idle units (and session children).
      Unit-scoped only — not hjem environment.sessionVariables / whole desktop.
    '';
  };

  users = lib.mkOption {
    type = lib.types.listOf lib.types.str;
    default = [ ];
    example = [ "luxus" ];
    description = "NixOS only: users added to video/render/input/uinput/audio.";
  };
}
