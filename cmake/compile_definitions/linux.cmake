# linux specific compile definitions

add_compile_definitions(POLARIS_PLATFORM="linux")

if(POLARIS_ENABLE_BROWSER_STREAM)
    list(APPEND POLARIS_DEFINITIONS POLARIS_ENABLE_BROWSER_STREAM=1)
endif()

if(POLARIS_ENABLE_WEBRTC)
    list(APPEND POLARIS_DEFINITIONS POLARIS_ENABLE_WEBRTC=1)
endif()

# AppImage
if(${POLARIS_BUILD_APPIMAGE})
    # use relative assets path for AppImage
    string(REPLACE "${CMAKE_INSTALL_PREFIX}" ".${CMAKE_INSTALL_PREFIX}" POLARIS_ASSETS_DIR_DEF ${POLARIS_ASSETS_DIR})
endif()

# Host integration files. Distribution packages install these to their live
# system paths so the package manager owns them; `--setup-host` then only has to
# apply them at runtime instead of copying anything into /etc. AppImage and
# portable builds have no package manager to own the files, so they keep the
# /etc install path.
find_package(Udev)
if(UDEV_FOUND AND UDEV_RULES_INSTALL_DIR)
    set(POLARIS_UDEV_RULES_DIR "${UDEV_RULES_INSTALL_DIR}" CACHE PATH
            "Directory holding Polaris' vendor udev rules")
else()
    set(POLARIS_UDEV_RULES_DIR "${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d" CACHE PATH
            "Directory holding Polaris' vendor udev rules")
endif()

find_package(Systemd)
if(SYSTEMD_FOUND AND SYSTEMD_MODULES_LOAD_DIR)
    set(POLARIS_MODULES_LOAD_DIR "${SYSTEMD_MODULES_LOAD_DIR}" CACHE PATH
            "Directory holding Polaris' modules-load configuration")
else()
    set(POLARIS_MODULES_LOAD_DIR "${CMAKE_INSTALL_PREFIX}/lib/modules-load.d" CACHE PATH
            "Directory holding Polaris' modules-load configuration")
endif()

message(STATUS "Polaris udev rules directory: ${POLARIS_UDEV_RULES_DIR}")
message(STATUS "Polaris modules-load directory: ${POLARIS_MODULES_LOAD_DIR}")

list(APPEND POLARIS_DEFINITIONS POLARIS_UDEV_RULES_DIR="${POLARIS_UDEV_RULES_DIR}")
list(APPEND POLARIS_DEFINITIONS POLARIS_MODULES_LOAD_DIR="${POLARIS_MODULES_LOAD_DIR}")

# cuda
set(CUDA_FOUND OFF)
set(POLARIS_CUDA_DISABLED_ON_NVIDIA_DETECTED OFF)
if(NOT POLARIS_ENABLE_CUDA AND NOT POLARIS_ALLOW_CUDA_DISABLED_ON_NVIDIA)
    find_program(POLARIS_DISABLED_CUDA_NVCC
            NAMES nvcc
            HINTS
                /usr/local/cuda/bin
                /usr/local/cuda-13.2/bin
                /usr/local/cuda-13.1/bin
                /usr/local/cuda-13.0/bin
                /usr/local/cuda-12.9/bin
                /usr/local/cuda-12.8/bin
                /usr/local/cuda-12.6/bin
                /usr/local/cuda-12.5/bin
                /usr/local/cuda-12.4/bin)
    find_program(POLARIS_DISABLED_CUDA_NVIDIA_SMI NAMES nvidia-smi)

    if(POLARIS_DISABLED_CUDA_NVCC AND POLARIS_DISABLED_CUDA_NVIDIA_SMI)
        set(POLARIS_CUDA_DISABLED_ON_NVIDIA_DETECTED ON)
        message(FATAL_ERROR
                "POLARIS_ENABLE_CUDA=OFF was requested, but this build host has both nvcc and nvidia-smi. "
                "CUDA-capable NVIDIA hosts should build Polaris with CUDA support so GPU-native/NVENC streaming remains available. "
                "If this CPU-only build is intentional, reconfigure with -DPOLARIS_ALLOW_CUDA_DISABLED_ON_NVIDIA=ON."
        )
    endif()
endif()

if(${POLARIS_ENABLE_CUDA})
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        set(CUDA_FOUND ON)
        enable_language(CUDA)

        message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")

        if(POLARIS_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES "${POLARIS_CUDA_ARCHITECTURES}")
            message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES} (POLARIS_CUDA_ARCHITECTURES override)")
        else()
            set(CMAKE_CUDA_ARCHITECTURES "")

            # https://docs.nvidia.com/cuda/archive/12.0.0/cuda-compiler-driver-nvcc/index.html
            if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
                list(APPEND CMAKE_CUDA_ARCHITECTURES 75 80 86 87 89 90)
            else()
                message(FATAL_ERROR
                        "Polaris requires a minimum CUDA Compiler version of 12.0.
                        Found version: ${CMAKE_CUDA_COMPILER_VERSION}"
                )
            endif()

            # https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html
            if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
                list(APPEND CMAKE_CUDA_ARCHITECTURES 100 101 120)
            endif()

            # https://docs.nvidia.com/cuda/archive/12.9.0/cuda-compiler-driver-nvcc/index.html
            if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.9)
                list(APPEND CMAKE_CUDA_ARCHITECTURES 103 121)
            endif()

            # https://docs.nvidia.com/cuda/archive/13.2.0/cuda-compiler-driver-nvcc/index.html
            if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.2)
                list(APPEND CMAKE_CUDA_ARCHITECTURES 88)
            endif()

            # https://docs.nvidia.com/cuda/archive/13.0.0/cuda-compiler-driver-nvcc/index.html
            if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
                list(REMOVE_ITEM CMAKE_CUDA_ARCHITECTURES 101)
                list(APPEND CMAKE_CUDA_ARCHITECTURES 110)
            else()
                list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52 53 60 61 62 70 72)
            endif()

            # sort the architectures
            list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)
            message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
        endif()

        # Enable fast math for CUDA kernels (safe for game streaming workloads)
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            string(APPEND CMAKE_CUDA_FLAGS " --use_fast_math")
        endif()

        # message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
        message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    elseif(${CUDA_FAIL_ON_MISSING})
        message(FATAL_ERROR
                "CUDA not found.
                If this is intentional, set '-DPOLARIS_ENABLE_CUDA=OFF' or '-DCUDA_FAIL_ON_MISSING=OFF'"
        )
    endif()
endif()
if(CUDA_FOUND)
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvfbc")
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cu"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/nvfbc/NvFBC.h")

    add_compile_definitions(POLARIS_BUILD_CUDA)
endif()

# CUDA interop and FFmpeg's Vulkan Video encoder both use the Vulkan loader.
if(CUDA_FOUND OR POLARIS_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    list(APPEND PLATFORM_LIBRARIES Vulkan::Vulkan)
endif()

# libdrm is required for DRM (KMS), Wayland, Vulkan, and Portal capture
if(${POLARIS_ENABLE_DRM} OR ${POLARIS_ENABLE_WAYLAND} OR ${POLARIS_ENABLE_VULKAN} OR ${POLARIS_ENABLE_PORTAL})
    find_package(LIBDRM REQUIRED)
else()
    set(LIBDRM_FOUND OFF)
endif()
if(LIBDRM_FOUND)
    include_directories(SYSTEM ${LIBDRM_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBDRM_LIBRARIES})
endif()

# drm
if(${POLARIS_ENABLE_DRM})
    find_package(LIBCAP REQUIRED)
else()
    set(LIBCAP_FOUND OFF)
endif()
if(LIBDRM_FOUND AND LIBCAP_FOUND)
    add_compile_definitions(POLARIS_BUILD_DRM)
    include_directories(SYSTEM ${LIBCAP_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBCAP_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/kmsgrab.cpp")
    list(APPEND POLARIS_DEFINITIONS EGL_NO_X11=1)
endif()

# evdev
include(dependencies/libevdev_Polaris)

# pipewire
#
# Portal capture also consumes PipeWire directly, even when native PipeWire
# audio is disabled, so detect the transport whenever either feature is on.
if(${POLARIS_ENABLE_PIPEWIRE} OR ${POLARIS_ENABLE_PORTAL})
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE libpipewire-0.3)
    endif()
else()
    set(PIPEWIRE_FOUND OFF)
endif()
if(PIPEWIRE_FOUND)
    include_directories(SYSTEM ${PIPEWIRE_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${PIPEWIRE_LIBRARIES})
    if(${POLARIS_ENABLE_PIPEWIRE})
        add_compile_definitions(POLARIS_BUILD_PIPEWIRE)
        message(STATUS "PipeWire support enabled")
    endif()
else()
    if(${POLARIS_ENABLE_PIPEWIRE})
        message(STATUS "PipeWire not found, using PulseAudio only")
    endif()
endif()

# xdg desktop portal (screen capture via org.freedesktop.portal.ScreenCast)
if(${POLARIS_ENABLE_PORTAL})
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GIO gio-2.0)
        pkg_check_modules(GIO_UNIX gio-unix-2.0)
    endif()
else()
    set(GIO_FOUND OFF)
    set(GIO_UNIX_FOUND OFF)
endif()
if(GIO_FOUND AND GIO_UNIX_FOUND)
    add_compile_definitions(POLARIS_BUILD_GIO)
    include_directories(SYSTEM ${GIO_INCLUDE_DIRS} ${GIO_UNIX_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${GIO_LIBRARIES} ${GIO_UNIX_LIBRARIES})
endif()
if(GIO_FOUND AND GIO_UNIX_FOUND AND PIPEWIRE_FOUND)
    add_compile_definitions(POLARIS_BUILD_PORTAL)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/portal_grab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/portal_capability.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/portal_session.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/pipewire_capture.cpp")
    message(STATUS "XDG Desktop Portal capture support enabled")
elseif(GIO_FOUND AND GIO_UNIX_FOUND)
    message(STATUS "XDG Desktop Portal capture not available (libpipewire-0.3 not found)")
else()
    message(STATUS "XDG Desktop Portal capture not available (gio-2.0 or gio-unix-2.0 not found)")
endif()

# vaapi
if(${POLARIS_ENABLE_VAAPI})
    find_package(Libva REQUIRED)
else()
    set(LIBVA_FOUND OFF)
endif()
if(LIBVA_FOUND)
    add_compile_definitions(POLARIS_BUILD_VAAPI)
    include_directories(SYSTEM ${LIBVA_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${LIBVA_LIBRARIES} ${LIBVA_DRM_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.cpp")
endif()

# Vulkan Video encoding through FFmpeg. Capture backends may provide either a
# GPU-resident DMA-BUF frame or a bounded RAM-upload path; runtime policy keeps
# automatic selection behind the route-specific live-frame safety gate.
if(${POLARIS_ENABLE_VULKAN})
    find_program(GLSLC_EXECUTABLE glslc)
    if(NOT GLSLC_EXECUTABLE)
        find_program(GLSLANG_EXECUTABLE glslangValidator)
    endif()
    if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_EXECUTABLE)
        message(FATAL_ERROR "Vulkan shader compiler not found (need glslc or glslangValidator)")
    endif()

    list(APPEND POLARIS_DEFINITIONS POLARIS_BUILD_VULKAN=1)
    include_directories(SYSTEM ${CMAKE_BINARY_DIR}/generated-src)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vulkan_encode.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vulkan_encode.cpp")

    set(VULKAN_SHADER_DIR "${CMAKE_BINARY_DIR}/generated-src/shaders")
    set(VULKAN_SHADER_SOURCE "${POLARIS_SOURCE_ASSETS_DIR}/linux/assets/shaders/vulkan/rgb2yuv.comp")
    set(VULKAN_SHADER_SPV "${VULKAN_SHADER_DIR}/rgb2yuv.spv")
    set(VULKAN_SHADER_DATA "${VULKAN_SHADER_DIR}/rgb2yuv.spv.inc")

    file(MAKE_DIRECTORY "${VULKAN_SHADER_DIR}")

    if(GLSLC_EXECUTABLE)
        add_custom_command(
                OUTPUT "${VULKAN_SHADER_SPV}"
                COMMAND ${GLSLC_EXECUTABLE} -O "${VULKAN_SHADER_SOURCE}" -o "${VULKAN_SHADER_SPV}"
                DEPENDS "${VULKAN_SHADER_SOURCE}"
                COMMENT "Compiling Vulkan shader rgb2yuv.comp (glslc)"
                VERBATIM)
    else()
        add_custom_command(
                OUTPUT "${VULKAN_SHADER_SPV}"
                COMMAND ${GLSLANG_EXECUTABLE} -V -o "${VULKAN_SHADER_SPV}" "${VULKAN_SHADER_SOURCE}"
                DEPENDS "${VULKAN_SHADER_SOURCE}"
                COMMENT "Compiling Vulkan shader rgb2yuv.comp (glslangValidator)"
                VERBATIM)
    endif()

    add_custom_command(
            OUTPUT "${VULKAN_SHADER_DATA}"
            COMMAND ${CMAKE_COMMAND} -DSPV_FILE=${VULKAN_SHADER_SPV} -DOUT_FILE=${VULKAN_SHADER_DATA}
                -P "${CMAKE_SOURCE_DIR}/cmake/scripts/binary_to_c.cmake"
            DEPENDS "${VULKAN_SHADER_SPV}"
            COMMENT "Generating C include from rgb2yuv.spv"
            VERBATIM)

    add_custom_target(vulkan_shaders
            DEPENDS "${VULKAN_SHADER_DATA}"
            COMMENT "Vulkan shader compilation")
    list(APPEND POLARIS_TARGET_DEPENDENCIES vulkan_shaders)
endif()

# wayland
if(${POLARIS_ENABLE_WAYLAND})
    find_package(Wayland REQUIRED)
else()
    set(WAYLAND_FOUND OFF)
endif()
if(WAYLAND_FOUND)
    add_compile_definitions(POLARIS_BUILD_WAYLAND)

    if(NOT POLARIS_SYSTEM_WAYLAND_PROTOCOLS)
        set(WAYLAND_PROTOCOLS_DIR "${CMAKE_SOURCE_DIR}/third-party/wayland-protocols")
    else()
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols REQUIRED)
    endif()

    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/xdg-output" xdg-output-unstable-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/linux-dmabuf" linux-dmabuf-unstable-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-foreign-toplevel-list" ext-foreign-toplevel-list-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-image-capture-source" ext-image-capture-source-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "staging/ext-image-copy-capture" ext-image-copy-capture-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/wlr-protocols" "unstable" wlr-screencopy-unstable-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/wlr-protocols" "unstable" wlr-virtual-pointer-unstable-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/src/platform/linux" "protocols" virtual-keyboard-unstable-v1)
    # KWin host capture (kwingrab) — private KDE protocols, not for third-party apps generally
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/src/platform/linux" "protocols" zkde-screencast-unstable-v1)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XKBCOMMON xkbcommon)
    endif()
    if(XKBCOMMON_FOUND)
        add_compile_definitions(POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT)
        include_directories(SYSTEM ${XKBCOMMON_INCLUDE_DIRS})
        link_directories(${XKBCOMMON_LIBRARY_DIRS})
        list(APPEND PLATFORM_LIBRARIES ${XKBCOMMON_LIBRARIES})
    else()
        message(STATUS "xkbcommon not found; labwc-local virtual input disabled")
    endif()

    include_directories(
            SYSTEM
            ${WAYLAND_INCLUDE_DIRS}
            ${CMAKE_BINARY_DIR}/generated-src
    )

    list(APPEND PLATFORM_LIBRARIES ${WAYLAND_LIBRARIES} gbm)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wlgrab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.cpp")

    # Portal remains available without native Wayland. These helpers, however,
    # include generated Wayland protocol headers and must only compile when both
    # the portal/PipeWire stack and Wayland are available.
    if(GIO_FOUND AND GIO_UNIX_FOUND AND PIPEWIRE_FOUND)
        list(APPEND PLATFORM_TARGET_FILES
                "${CMAKE_SOURCE_DIR}/src/platform/linux/cage_screencopy.h"
                "${CMAKE_SOURCE_DIR}/src/platform/linux/cage_screencopy.cpp"
                "${CMAKE_SOURCE_DIR}/src/platform/linux/kwingrab.h"
                "${CMAKE_SOURCE_DIR}/src/platform/linux/kwingrab.cpp")
    endif()
endif()

# x11
if(${POLARIS_ENABLE_X11})
    find_package(X11 REQUIRED)
else()
    set(X11_FOUND OFF)
endif()
if(X11_FOUND)
    add_compile_definitions(POLARIS_BUILD_X11)
    include_directories(SYSTEM ${X11_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${X11_LIBRARIES})
    # The private-session attach probe talks to Xwayland over xcb rather than
    # Xlib, whose default I/O error handler exits the process when a display
    # goes away (issue #415).
    if(X11_xcb_FOUND)
        add_compile_definitions(POLARIS_BUILD_X11_XCB)
        list(APPEND PLATFORM_LIBRARIES ${X11_xcb_LIB})
    else()
        message(STATUS "libxcb not found; private-session Xwayland attach signal disabled")
    endif()
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.cpp")
endif()

if(NOT CUDA_FOUND
        AND NOT WAYLAND_FOUND
        AND NOT X11_FOUND
        AND NOT (LIBDRM_FOUND AND LIBCAP_FOUND)
        AND NOT LIBVA_FOUND
        AND NOT (GIO_FOUND AND GIO_UNIX_FOUND AND PIPEWIRE_FOUND))
    message(FATAL_ERROR "Couldn't find either cuda, wayland, x11, (libdrm and libcap), libva, or Portal/PipeWire capture support")
endif()

# tray icon
if(${POLARIS_ENABLE_TRAY})
    pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
    if(APPINDICATOR_FOUND)
        list(APPEND POLARIS_DEFINITIONS TRAY_AYATANA_APPINDICATOR=1)
    else()
        pkg_check_modules(APPINDICATOR appindicator3-0.1)
        if(APPINDICATOR_FOUND)
            list(APPEND POLARIS_DEFINITIONS TRAY_LEGACY_APPINDICATOR=1)
        endif ()
    endif()
    pkg_check_modules(LIBNOTIFY libnotify)
    if(NOT APPINDICATOR_FOUND OR NOT LIBNOTIFY_FOUND)
        message(STATUS "APPINDICATOR_FOUND: ${APPINDICATOR_FOUND}")
        message(STATUS "LIBNOTIFY_FOUND: ${LIBNOTIFY_FOUND}")
        message(FATAL_ERROR "Couldn't find either appindicator or libnotify")
    else()
        include_directories(SYSTEM ${APPINDICATOR_INCLUDE_DIRS} ${LIBNOTIFY_INCLUDE_DIRS})
        link_directories(${APPINDICATOR_LIBRARY_DIRS} ${LIBNOTIFY_LIBRARY_DIRS})

        list(APPEND PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray_linux.c")
        list(APPEND POLARIS_EXTERNAL_LIBRARIES ${APPINDICATOR_LIBRARIES} ${LIBNOTIFY_LIBRARIES})
    endif()

    set(POLARIS_TRAY_PREFIX "polaris")
    list(APPEND POLARIS_DEFINITIONS POLARIS_TRAY_PREFIX="${POLARIS_TRAY_PREFIX}")
else()
    set(POLARIS_TRAY 0)
    message(STATUS "Tray icon disabled")
endif()

# These need to be set before adding the inputtino subdirectory in order for them to be picked up
set(LIBEVDEV_CUSTOM_INCLUDE_DIR "${EVDEV_INCLUDE_DIR}")
set(LIBEVDEV_CUSTOM_LIBRARY "${EVDEV_LIBRARY}")

add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/inputtino")
set_target_properties(libinputtino PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
list(APPEND POLARIS_EXTERNAL_LIBRARIES inputtino::libinputtino)
file(GLOB_RECURSE INPUTTINO_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.h
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.cpp)
list(APPEND PLATFORM_TARGET_FILES ${INPUTTINO_SOURCES})

# build libevdev before the libinputtino target
if(EXTERNAL_PROJECT_LIBEVDEV_USED)
    add_dependencies(libinputtino libevdev)
endif()

# AppImage
if (${POLARIS_BUILD_APPIMAGE})
    list(APPEND POLARIS_DEFINITIONS POLARIS_BUILD_APPIMAGE=1)
endif ()

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/libva_compat.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/virtual_display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/virtual_display.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_manager.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_manager.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/private_session_input.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/private_session_input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/private_session_attach.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/private_session_attach.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/input/input_group_access.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/input/input_group_access.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/cage_display_router.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/cage_display_router.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/display_topology.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/display_topology.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_path.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_path.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_display_policy.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_display_policy.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_runtime.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/gamescope_process.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/gamescope_process.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_runtime_labwc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/stream_runtime_gamescope.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_launch_linux.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_launch_linux.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_media.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/session_media.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/portal_session.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/egl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/gl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/EGL/eglplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/KHR/khrplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/gl.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/egl.h")

if(POLARIS_HAVE_LIBVA_MAP_BUFFER2)
    list(REMOVE_ITEM PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/src/platform/linux/libva_compat.cpp")
endif()

list(APPEND PLATFORM_LIBRARIES
        dl
        pulse
        pulse-simple)

include_directories(
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party/glad/include")
