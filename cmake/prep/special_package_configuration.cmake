if(UNIX)
    if(${POLARIS_CONFIGURE_HOMEBREW})
        configure_file(packaging/polaris.rb polaris.rb @ONLY)
    endif()
endif()

if(APPLE)
    if(${POLARIS_CONFIGURE_PORTFILE})
        configure_file(packaging/macos/Portfile Portfile @ONLY)
    endif()
elseif(UNIX)
    # configure the .desktop file
    if(${POLARIS_BUILD_APPIMAGE})
        configure_file(packaging/linux/AppImage/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
    else()
        configure_file(packaging/linux/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
        configure_file(packaging/linux/${PROJECT_FQDN}.terminal.desktop ${PROJECT_FQDN}.terminal.desktop @ONLY)
    endif()

    # configure metadata file
    configure_file(packaging/linux/${PROJECT_FQDN}.metainfo.xml ${PROJECT_FQDN}.metainfo.xml @ONLY)

    # configure service
    configure_file(packaging/linux/polaris.service.in polaris.service @ONLY)

    # configure the arch linux pkgbuild
    if(${POLARIS_CONFIGURE_PKGBUILD})
        configure_file(packaging/linux/Arch/PKGBUILD PKGBUILD @ONLY)
        configure_file(packaging/linux/Arch/polaris.install polaris.install @ONLY)
        # The split package names this one, and makepkg reads it from beside the PKGBUILD.
        configure_file(packaging/linux/Arch/polaris-kms.install polaris-kms.install @ONLY)
    endif()

    # configure the dedicated SteamOS package files
    if(${POLARIS_CONFIGURE_STEAMOS_PKGBUILD})
        # Each SteamOS release gets its own package, built in a root bootstrapped from that release's
        # repositories (scripts/ci/run-steamos-build.sh). labwc is known not to install on 3.8 and
        # has not been tried on 3.9, so the optional dependency says only what is known.
        if(POLARIS_STEAMOS_TARGET STREQUAL "3.8")
            set(POLARIS_STEAMOS_RUNTIME_DEPENDS "'gcc-libs'")
            set(POLARIS_STEAMOS_LABWC_OPTDEPEND
                    "labwc: Private Stream paths; not installable on SteamOS 3.8, see docs/steamos.md")
        elseif(POLARIS_STEAMOS_TARGET STREQUAL "3.9")
            # GCC 16's runtime libraries have separate owners on SteamOS 3.9. gcc-libs is
            # only an umbrella there; declaring the owners also lets namcap verify the links.
            set(POLARIS_STEAMOS_RUNTIME_DEPENDS "'libgcc'\n  'libstdc++'")
            set(POLARIS_STEAMOS_LABWC_OPTDEPEND
                    "labwc: Private Stream paths; not verified on SteamOS 3.9, see docs/steamos.md")
        else()
            message(FATAL_ERROR "POLARIS_STEAMOS_TARGET must be 3.8 or 3.9, not '${POLARIS_STEAMOS_TARGET}'")
        endif()
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/packaging/linux/SteamOS)
        configure_file(
                packaging/linux/SteamOS/PKGBUILD
                ${CMAKE_BINARY_DIR}/packaging/linux/SteamOS/PKGBUILD
                @ONLY)
        configure_file(
                packaging/linux/SteamOS/polaris.install
                ${CMAKE_BINARY_DIR}/packaging/linux/SteamOS/polaris.install
                @ONLY)
        configure_file(
                packaging/linux/SteamOS/polaris-kms.install
                ${CMAKE_BINARY_DIR}/packaging/linux/SteamOS/polaris-kms.install
                @ONLY)
    endif()
endif()

# return if configure only is set
if(${POLARIS_CONFIGURE_ONLY})
    # message
    message(STATUS "POLARIS_CONFIGURE_ONLY: ON, exiting...")
    set(END_BUILD ON)
else()
    set(END_BUILD OFF)
endif()
