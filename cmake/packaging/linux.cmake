# linux specific packaging

install(FILES "${POLARIS_SPACES_INPUT}" "${POLARIS_SPACES_WORKER}" "${POLARIS_SPACES_RULE}"
              "${CMAKE_BINARY_DIR}/generated/polaris_spaces_version.cil"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/polaris/multiseat/security")
if(NOT POLARIS_BUILD_APPIMAGE)
    install(PROGRAMS "${CMAKE_BINARY_DIR}/generated/polaris-spaces-setup"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # The polkit actions the Spaces page asks for; they only name the helper installed above.
    install(FILES "${CMAKE_BINARY_DIR}/generated/${POLARIS_POLKIT_POLICY_NAME}"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions")
endif()

install(FILES "${POLARIS_STEAM_SECCOMP_SOURCE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/polaris/multiseat"
        RENAME "${POLARIS_STEAM_SECCOMP_NAME}")
install(FILES "${CMAKE_SOURCE_DIR}/containers/multiseat/seccomp/LICENSE.moby"
              "${CMAKE_SOURCE_DIR}/containers/multiseat/seccomp/upstream.json"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/polaris/multiseat/seccomp-provenance")

install(DIRECTORY "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${POLARIS_ASSETS_DIR}")

# The bundled copies under ${POLARIS_ASSETS_DIR} are what `--setup-host` installs
# from when Polaris runs portable or from an AppImage, and what a build directory
# run reads. Distribution packages additionally install them to their live system
# paths below, so the package manager owns the files and removes them on uninstall.
install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.rules"
        DESTINATION "${POLARIS_ASSETS_DIR}/udev/rules.d")
install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.conf"
        DESTINATION "${POLARIS_ASSETS_DIR}/modules-load.d")

# Reference copies of the gamescope_stream helpers. At launch Polaris compares
# the polaris-gamescope-session it resolved against these, so a helper left by
# an older scripts/install run is reported as stale instead of debugged as a
# Polaris bug. They are executable so an AppImage or a build-directory run,
# which has no launcher beside the binary or on PATH, can nest through them.
install(PROGRAMS "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-session.sh"
                 "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-runtime-lib.sh"
        DESTINATION "${POLARIS_ASSETS_DIR}/gamescope")

# Host integration files at their live paths, for everything that is not an
# AppImage. An AppImage cannot own paths outside its mount, so it keeps relying
# on `polaris --setup-host` to place these under /etc.
if(NOT ${POLARIS_BUILD_APPIMAGE})
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.rules"
            DESTINATION "${POLARIS_UDEV_RULES_DIR}")
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.conf"
            DESTINATION "${POLARIS_MODULES_LOAD_DIR}")

    # gamescope_stream resolves this launcher by name at runtime. Keep the
    # shared ownership library beside it so the launcher's default lookup is
    # package-owned on DEB, RPM, Arch, and SteamOS installs.
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-session.sh"
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
            RENAME "polaris-gamescope-session")
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-runtime-lib.sh"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.rules"
        DESTINATION "${CMAKE_BINARY_DIR}/assets/udev/rules.d")
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.conf"
        DESTINATION "${CMAKE_BINARY_DIR}/assets/modules-load.d")
file(COPY "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-session.sh"
          "${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-runtime-lib.sh"
        DESTINATION "${CMAKE_BINARY_DIR}/assets/gamescope")
# use symbolic link for shaders directory
file(CREATE_LINK "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/shaders"
        "${CMAKE_BINARY_DIR}/assets/shaders" COPY_ON_ERROR SYMBOLIC)

if(${POLARIS_BUILD_APPIMAGE})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/polaris.service"
            DESTINATION "${POLARIS_ASSETS_DIR}/systemd/user")
else()
    find_package(Systemd)
    if(SYSTEMD_FOUND)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/polaris.service"
                DESTINATION "${SYSTEMD_USER_UNIT_INSTALL_DIR}")
    endif()
endif()

# The DRM/KMS capture helper.
#
# Capture through DRM/KMS needs CAP_SYS_ADMIN. Applying it to /usr/bin/polaris meant every install
# and every update replaced the binary that held it, so KMS capture stopped until someone ran
# --enable-kms again, and on an image-based host setcap on /usr is refused outright. So the
# capability belongs to a file the package manager owns, in a package of its own that nobody
# downloads unless they want it: the binary is 31 MiB, and most hosts never capture this way.
#
# 0750 root:polaris-kms rather than 0755, because a capability on a world-executable file hands
# CAP_SYS_ADMIN to every local account. The cost is that a session picks up its groups at login, so
# --enable-kms has to say that KMS capture starts working after a logout.
if(NOT ${POLARIS_BUILD_APPIMAGE})
    install(PROGRAMS "$<TARGET_FILE:polaris>"
            DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/polaris"
            RENAME "polaris-kms"
            COMPONENT kms)
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/polaris-kms.sysusers"
            DESTINATION "lib/sysusers.d"
            RENAME "polaris-kms.conf"
            COMPONENT kms)
endif()

# Post install
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/postinst")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/postinst")

# Removal. One script for both formats, as with postinst above, because it distinguishes a removal
# from an upgrade by reading $1, which dpkg and rpm both set and merely spell differently.
# It exists for one reason that has no alternative: polaris-spaces-setup is the only thing that can
# remove the SELinux policies it installed, and it ships inside this package, so after removal they
# cannot be removed at all. See polaris#63.
list(APPEND CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/prerm")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/prerm")

# Two packages out of one build. The names are pinned per component because CPack otherwise
# derives them from the component, and because the release picks assets by name.
set(CPACK_COMPONENTS_ALL polaris kms)
set(CPACK_COMPONENTS_GROUPING IGNORE)

set(CPACK_RPM_COMPONENT_INSTALL ON)
# The main package keeps the file name it has always had, and the helper gets one of its own.
# Without this CPack appends the component, so the release would have to guess from a sorted
# listing, where Polaris-kms sorts ahead of Polaris and the helper would ship as if it were Polaris.
set(CPACK_RPM_POLARIS_FILE_NAME "Polaris.rpm")
set(CPACK_RPM_KMS_FILE_NAME "Polaris-kms.rpm")
set(CPACK_RPM_POLARIS_PACKAGE_NAME "polaris")
set(CPACK_RPM_KMS_PACKAGE_NAME "polaris-kms")
set(CPACK_RPM_KMS_PACKAGE_SUMMARY "DRM/KMS capture helper for Polaris")
set(CPACK_RPM_KMS_PACKAGE_DESCRIPTION
        "The privileged helper Polaris runs to capture through DRM/KMS. Install it only if you \
capture that way; every other capture path works without it.")
# Exactly this version of Polaris, so the two can never disagree about what the helper is.
set(CPACK_RPM_KMS_PACKAGE_REQUIRES "polaris = ${CPACK_PACKAGE_VERSION}")
# rpm carries file capabilities in package metadata, which is the whole point: they land on disk on
# every install and every update, including where a runtime setcap would be refused.
set(CPACK_RPM_KMS_USER_FILELIST
        "%caps(cap_sys_admin=ep) %attr(0750,root,polaris-kms) ${CMAKE_INSTALL_FULL_LIBEXECDIR}/polaris/polaris-kms")

# Dependencies
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_POLARIS_FILE_NAME "Polaris.deb")
set(CPACK_DEBIAN_KMS_FILE_NAME "Polaris-kms.deb")
set(CPACK_DEBIAN_POLARIS_PACKAGE_NAME "polaris")
set(CPACK_DEBIAN_KMS_PACKAGE_NAME "polaris-kms")
set(CPACK_DEBIAN_KMS_PACKAGE_SHLIBDEPS OFF)
# Keep the main runtime dependencies on Polaris. The helper's maintainer script
# additionally needs setcap and groupadd even on a minimal installation.
set(CPACK_DEBIAN_KMS_PACKAGE_DEPENDS "polaris (= ${CPACK_PACKAGE_VERSION}), libcap2-bin, passwd")
# Its own scriptlet: the main one talks about --setup-host, and this one has real work to do.
# dpkg runs a maintainer script only under its exact name, and CONTROL_EXTRA keeps the basename,
# so this one lives in a directory of its own rather than being called postinst-kms and never running.
set(CPACK_DEBIAN_KMS_PACKAGE_CONTROL_EXTRA
        "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/kms/postinst")
set(CPACK_DEBIAN_KMS_DESCRIPTION
        "DRM/KMS capture helper for Polaris")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
            ${CPACK_DEB_PLATFORM_PACKAGE_DEPENDS} \
            bash, \
            python3, \
            debianutils, \
            grim, \
            labwc, \
            libcap2, \
            libcurl4, \
            libdrm2, \
            libgbm1, \
            libei1, \
            libevdev2, \
            libnuma1, \
            libopus0, \
            libpulse0, \
            libva2, \
            libva-drm2, \
            libwayland-client0, \
            libx11-6, \
            miniupnpc, \
            openssl | libssl3, \
            wlr-randr, \
            x11-utils, \
            xwayland")
set(CPACK_RPM_PACKAGE_REQUIRES "\
            ${CPACK_RPM_PLATFORM_PACKAGE_REQUIRES} \
            bash, \
            python3, \
            grim, \
            labwc, \
            libcap >= 2.22, \
            libcurl >= 7.0, \
            libdrm >= 2.4.97, \
            libei >= 1.0, \
            libevdev >= 1.5.6, \
            libopusenc >= 0.2.1, \
            libva >= 2.14.0, \
            libwayland-client >= 1.20.0, \
            libX11 >= 1.7.3.1, \
            mesa-libgbm >= 25.0.7, \
            miniupnpc >= 2.2.4, \
            numactl-libs >= 2.0.14, \
            openssl >= 3.0.2, \
            pulseaudio-libs >= 10.0, \
            which >= 2.21, \
            wlr-randr, \
            xdpyinfo, \
            xorg-x11-server-Xwayland")

if(NOT BOOST_USE_STATIC)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                libboost-filesystem${Boost_VERSION}, \
                libboost-locale${Boost_VERSION}, \
                libboost-log${Boost_VERSION}, \
                libboost-program-options${Boost_VERSION}")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                ${CPACK_RPM_PACKAGE_REQUIRES}, \
                boost-filesystem >= ${Boost_VERSION}, \
                boost-locale >= ${Boost_VERSION}, \
                boost-log >= ${Boost_VERSION}, \
                boost-program-options >= ${Boost_VERSION}")
endif()

# This should automatically figure out dependencies, doesn't work with the current config
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

# application icon
install(FILES "${CMAKE_SOURCE_DIR}/polaris.svg"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")

# tray icon
if(${POLARIS_TRAY} STREQUAL 1)
    install(FILES "${CMAKE_SOURCE_DIR}/polaris.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
            RENAME "polaris-tray.svg")
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/common/assets/web/public/images/polaris-playing.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/common/assets/web/public/images/polaris-pausing.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")
    install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/common/assets/web/public/images/polaris-locked.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")

    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                    ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                    libayatana-appindicator3-1, \
                    libnotify4")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                    ${CPACK_RPM_PACKAGE_REQUIRES}, \
                    libappindicator-gtk3 >= 12.10.0")
endif()

# desktop file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
if(NOT ${POLARIS_BUILD_APPIMAGE})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.terminal.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
endif()

# metadata file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
