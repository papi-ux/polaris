# Private ScreenCast portal for a gamescope Wayland socket (Jovian-Experiments).
# Portal capture can target gamescope-0 instead of the desktop compositor.
{
  lib,
  stdenv,
  fetchFromGitHub,
  meson,
  ninja,
  pkg-config,
  rustc,
  cargo,
  rustPlatform,
  systemd,
  dbus,
  pipewire,
  makeWrapper,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "xdg-desktop-portal-gamescope";
  version = "0.1.33.412a4bf";

  src = fetchFromGitHub {
    owner = "Jovian-Experiments";
    repo = "xdg-desktop-portal-gamescope";
    rev = "412a4bff892bdb5726a549d03b11e6ce2f8e8152";
    hash = "sha256-WmovgfLZQwa+a04FiAlNDwom/xDl8VURn/gmMit8Nvk=";
  };

  cargoDeps = rustPlatform.fetchCargoVendor {
    inherit (finalAttrs) src;
    hash = "sha256-yh/sTiSgzuVJZquMrDheMAHcE7kczOx/v14AHuAYBOs=";
  };

  patches = [
    # IceDOS: negotiate stream size with the client (portal → host).
    ../../patches/xdg-desktop-portal-gamescope/01-fix-stream-size.patch
  ];

  env.PKG_CONFIG_DBUS_1_SESSION_BUS_SERVICES_DIR = "${placeholder "out"}/share/dbus-1/services";

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    rustc
    cargo
    rustPlatform.cargoSetupHook
    makeWrapper
  ];

  buildInputs = [
    systemd
    dbus
  ];

  # Bind the portal process to the idle gamescope Wayland socket name.
  postInstall = ''
    wrapProgram $out/libexec/xdg-desktop-portal-gamescope \
      --set-default WAYLAND_DISPLAY gamescope-0 \
      --prefix PATH : ${lib.makeBinPath [ pipewire ]}

    mkdir -p $out/share/xdg-desktop-portal/portals
    cat > $out/share/xdg-desktop-portal/portals/gamescope.portal <<EOF
    [portal]
    DBusName=org.freedesktop.impl.portal.desktop.gamescope
    Interfaces=org.freedesktop.impl.portal.Access;org.freedesktop.impl.portal.ScreenCast;org.freedesktop.impl.portal.Screenshot;
    UseIn=gamescope
    EOF
  '';

  meta = {
    description = "xdg-desktop-portal backend for gamescope screencast (HDR headless sessions)";
    homepage = "https://github.com/Jovian-Experiments/xdg-desktop-portal-gamescope";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "xdg-desktop-portal-gamescope";
  };
})
