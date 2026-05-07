# Publisher Metadata
set(POLARIS_PUBLISHER_NAME "Polaris Project"
        CACHE STRING "The name of the publisher (or fork developer) of the application.")
set(POLARIS_PUBLISHER_WEBSITE "https://github.com/papi-ux/polaris"
        CACHE STRING "The URL of the publisher's website.")
set(POLARIS_PUBLISHER_ISSUE_URL "https://github.com/papi-ux/polaris/issues"
        CACHE STRING "The URL of the publisher's support site or issue tracker.
        If you provide a modified version of Polaris, we kindly request that you use your own url.")

option(BUILD_DOCS "Build documentation" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_TEST_COVERAGE "Enable gcov coverage instrumentation for tests." OFF)

option(POLARIS_ENABLE_NATIVE_ARCH
        "Use -march=native for optimized local builds. Disable for portable/distributed binaries." OFF)
option(NPM_OFFLINE "Use offline npm packages. You must ensure packages are in your npm cache." OFF)

option(BUILD_WERROR "Enable -Werror flag." OFF)

# if this option is set, the build will exit after configuring special package configuration files
option(POLARIS_CONFIGURE_ONLY "Configure special files only, then exit." OFF)

option(POLARIS_ENABLE_TRAY "Enable system tray icon." ON)

option(POLARIS_SYSTEM_WAYLAND_PROTOCOLS "Use system installation of wayland-protocols rather than the submodule." OFF)

if(APPLE)
    option(BOOST_USE_STATIC "Use static boost libraries." OFF)
else()
    option(BOOST_USE_STATIC "Use static boost libraries." ON)
endif()

option(CUDA_FAIL_ON_MISSING "Fail the build if CUDA is not found." ON)
option(CUDA_INHERIT_COMPILE_OPTIONS
        "When building CUDA code, inherit compile options from the the main project. You may want to disable this if
        your IDE throws errors about unknown flags after running cmake." ON)

if(UNIX)
    option(POLARIS_BUILD_HOMEBREW
            "Enable a Homebrew build." OFF)
    option(POLARIS_CONFIGURE_HOMEBREW
            "Configure Homebrew formula. Recommended to use with POLARIS_CONFIGURE_ONLY" OFF)
endif()

if(APPLE)
    option(POLARIS_CONFIGURE_PORTFILE
            "Configure macOS Portfile. Recommended to use with POLARIS_CONFIGURE_ONLY" OFF)
    option(POLARIS_PACKAGE_MACOS
            "Should only be used when creating a macOS package/dmg." OFF)
elseif(UNIX)  # Linux
    option(POLARIS_BUILD_APPIMAGE
            "Enable an AppImage build." OFF)
    option(POLARIS_CONFIGURE_PKGBUILD
            "Configure files required for AUR. Recommended to use with POLARIS_CONFIGURE_ONLY" OFF)

    # Linux capture methods
    option(POLARIS_ENABLE_CUDA
            "Enable cuda specific code." ON)
    option(POLARIS_ENABLE_DRM
            "Enable KMS grab if available." ON)
    option(POLARIS_ENABLE_VAAPI
            "Enable building vaapi specific code." ON)
    option(POLARIS_ENABLE_WAYLAND
            "Enable building wayland specific code." ON)
    option(POLARIS_ENABLE_X11
            "Enable X11 grab if available." ON)
    option(POLARIS_ENABLE_PIPEWIRE
            "Enable native PipeWire audio support." ON)
    option(POLARIS_ENABLE_PORTAL
            "Enable XDG Desktop Portal screen capture if available." ON)
    option(POLARIS_ENABLE_BROWSER_STREAM
            "Enable experimental Browser Stream WebTransport support." OFF)
    option(POLARIS_ENABLE_WEBRTC
            "Deprecated alias for POLARIS_ENABLE_BROWSER_STREAM." OFF)
    if(POLARIS_ENABLE_WEBRTC)
        message(DEPRECATION "POLARIS_ENABLE_WEBRTC is deprecated; use POLARIS_ENABLE_BROWSER_STREAM instead.")
        set(POLARIS_ENABLE_BROWSER_STREAM ON CACHE BOOL
                "Enable experimental Browser Stream WebTransport support." FORCE)
    endif()
endif()
