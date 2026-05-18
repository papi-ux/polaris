# load common dependencies
# this file will also load platform specific dependencies

# boost, this should be before Simple-Web-Server as it also depends on boost
include(dependencies/Boost_Polaris)

# submodules
# moonlight common library
set(ENET_NO_INSTALL ON CACHE BOOL "Don't install any libraries built for enet")
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet")

# web server
set(POLARIS_BUILD_TESTING_PRE_SWS "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable vendored dependency tests during Polaris builds" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")
set(BUILD_TESTING "${POLARIS_BUILD_TESTING_PRE_SWS}" CACHE BOOL "Restore parent BUILD_TESTING setting after vendored dependency configuration" FORCE)
unset(POLARIS_BUILD_TESTING_PRE_SWS)

# libdisplaydevice
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdisplaydevice")

# common dependencies
include("${CMAKE_MODULE_PATH}/dependencies/nlohmann_json.cmake")
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

# miniupnp
pkg_check_modules(MINIUPNP miniupnpc REQUIRED)
include_directories(SYSTEM ${MINIUPNP_INCLUDE_DIRS})

# ffmpeg pre-compiled binaries
if(WIN32)
    set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
elseif(UNIX AND NOT APPLE)
    set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm)
    if(POLARIS_ENABLE_X11)
        list(APPEND FFMPEG_PLATFORM_LIBRARIES va-x11 X11)
    endif()
endif()

if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    set(FFMPEG_PREPARED_BINARIES
            "${CMAKE_SOURCE_DIR}/third-party/build-deps/dist/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

    # check if the directory exists
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}")
        message(FATAL_ERROR
                "FFmpeg pre-compiled binaries not found at ${FFMPEG_PREPARED_BINARIES}. \
                Please consider contributing to the LizardByte/build-deps repository. \
                Optionally, you can use the FFMPEG_PREPARED_BINARIES option to specify the path to the \
                system-installed FFmpeg libraries")
    endif()
endif()

set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a")
foreach(FFMPEG_OPTIONAL_LIBRARY libSvtAv1Enc.a libx264.a libx265.a libhdr10plus.a)
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/${FFMPEG_OPTIONAL_LIBRARY}")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/${FFMPEG_OPTIONAL_LIBRARY}")
    endif()
endforeach()
list(APPEND FFMPEG_LIBRARIES ${FFMPEG_PLATFORM_LIBRARIES})

set(FFMPEG_INCLUDE_DIRS
        "${FFMPEG_PREPARED_BINARIES}/include")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    if(APPLE)
        include("${CMAKE_MODULE_PATH}/dependencies/macos.cmake")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
    endif()
endif()
