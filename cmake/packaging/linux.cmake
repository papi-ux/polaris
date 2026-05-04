# linux specific packaging

install(DIRECTORY "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${POLARIS_ASSETS_DIR}")
install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.rules"
        DESTINATION "${POLARIS_ASSETS_DIR}/udev/rules.d")
install(FILES "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.conf"
        DESTINATION "${POLARIS_ASSETS_DIR}/modules-load.d")

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.rules"
        DESTINATION "${CMAKE_BINARY_DIR}/assets/udev/rules.d")
file(COPY "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/60-polaris.conf"
        DESTINATION "${CMAKE_BINARY_DIR}/assets/modules-load.d")
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

# Post install
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/postinst")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${POLARIS_SOURCE_ASSETS_DIR}/linux/misc/postinst")

# Dependencies
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
            ${CPACK_DEB_PLATFORM_PACKAGE_DEPENDS} \
            debianutils, \
            grim, \
            labwc, \
            libcap2, \
            libcurl4, \
            libdrm2, \
            libgbm1, \
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
            grim, \
            labwc, \
            libcap >= 2.22, \
            libcurl >= 7.0, \
            libdrm >= 2.4.97, \
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
