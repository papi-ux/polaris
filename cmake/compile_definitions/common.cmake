# common compile definitions
# this file will also load platform specific definitions

list(APPEND POLARIS_COMPILE_OPTIONS -Wall -Wno-sign-compare)
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap

# Release build optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # CPU architecture tuning
    if(POLARIS_ENABLE_NATIVE_ARCH)
        list(APPEND POLARIS_COMPILE_OPTIONS -march=native)
        message(STATUS "Using -march=native for optimized local build")
    endif()

    # Enable dead code elimination via linker gc-sections
    list(APPEND POLARIS_COMPILE_OPTIONS -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)

    # Reduce symbol table bloat and enable hidden visibility optimization
    list(APPEND POLARIS_COMPILE_OPTIONS -fvisibility=hidden -fvisibility-inlines-hidden)

    # Enable loop unrolling for performance-critical paths
    list(APPEND POLARIS_COMPILE_OPTIONS -funroll-loops)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options

    # GCC 12 and higher will complain about maybe-uninitialized
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        list(APPEND POLARIS_COMPILE_OPTIONS -Wno-maybe-uninitialized)

        # Disable the bogus warning that may prevent compilation (only for GCC 12).
        # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            list(APPEND POLARIS_COMPILE_OPTIONS -Wno-restrict)
        endif()
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND POLARIS_COMPILE_OPTIONS -Wno-uninitialized)
endif()
if(BUILD_WERROR)
    list(APPEND POLARIS_COMPILE_OPTIONS -Werror)
endif()

# setup assets directory
if(NOT POLARIS_ASSETS_DIR)
    set(POLARIS_ASSETS_DIR "assets")
endif()

# platform specific compile definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/compile_definitions/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/compile_definitions/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/compile_definitions/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/compile_definitions/linux.cmake)
    endif()
endif()

include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nv-codec-headers/include")
file(GLOB NVENC_SOURCES CONFIGURE_DEPENDS "src/nvenc/*.cpp" "src/nvenc/*.h")
list(APPEND PLATFORM_TARGET_FILES ${NVENC_SOURCES})

set(POLARIS_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/adaptive_bitrate.h"
        "${CMAKE_SOURCE_DIR}/src/adaptive_bitrate.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Input.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Rtsp.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Video.h"
        "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray.h"
        "${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        "${CMAKE_SOURCE_DIR}/src/upnp.h"
        "${CMAKE_SOURCE_DIR}/src/cbs.cpp"
        "${CMAKE_SOURCE_DIR}/src/utility.h"
        "${CMAKE_SOURCE_DIR}/src/uuid.h"
        "${CMAKE_SOURCE_DIR}/src/client_profiles.h"
        "${CMAKE_SOURCE_DIR}/src/client_profiles.cpp"
        "${CMAKE_SOURCE_DIR}/src/device_db.h"
        "${CMAKE_SOURCE_DIR}/src/device_db.cpp"
        "${CMAKE_SOURCE_DIR}/src/ai_optimizer.h"
        "${CMAKE_SOURCE_DIR}/src/ai_optimizer.cpp"
        "${CMAKE_SOURCE_DIR}/src/game_classifier.h"
        "${CMAKE_SOURCE_DIR}/src/game_classifier.cpp"
        "${CMAKE_SOURCE_DIR}/src/config.h"
        "${CMAKE_SOURCE_DIR}/src/config.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device.h"
        "${CMAKE_SOURCE_DIR}/src/display_device.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.h"
        "${CMAKE_SOURCE_DIR}/src/file_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/file_handler.h"
        "${CMAKE_SOURCE_DIR}/src/globals.cpp"
        "${CMAKE_SOURCE_DIR}/src/globals.h"
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging.h"
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/main.h"
        "${CMAKE_SOURCE_DIR}/src/crypto.cpp"
        "${CMAKE_SOURCE_DIR}/src/crypto.h"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.h"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.cpp"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.h"
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/confighttp.h"
        "${CMAKE_SOURCE_DIR}/src/confighttp_validation.cpp"
        "${CMAKE_SOURCE_DIR}/src/confighttp_validation.h"
        "${CMAKE_SOURCE_DIR}/src/rtsp.cpp"
        "${CMAKE_SOURCE_DIR}/src/rtsp.h"
        "${CMAKE_SOURCE_DIR}/src/stream.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream.h"
        "${CMAKE_SOURCE_DIR}/src/video.cpp"
        "${CMAKE_SOURCE_DIR}/src/video.h"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.cpp"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.h"
        "${CMAKE_SOURCE_DIR}/src/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/input.h"
        "${CMAKE_SOURCE_DIR}/src/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio.h"
        "${CMAKE_SOURCE_DIR}/src/platform/common.h"
        "${CMAKE_SOURCE_DIR}/src/process.cpp"
        "${CMAKE_SOURCE_DIR}/src/process.h"
        "${CMAKE_SOURCE_DIR}/src/network.cpp"
        "${CMAKE_SOURCE_DIR}/src/network.h"
        "${CMAKE_SOURCE_DIR}/src/wol.h"
        "${CMAKE_SOURCE_DIR}/src/wol.cpp"
        "${CMAKE_SOURCE_DIR}/src/move_by_copy.h"
        "${CMAKE_SOURCE_DIR}/src/system_tray.cpp"
        "${CMAKE_SOURCE_DIR}/src/system_tray.h"
        "${CMAKE_SOURCE_DIR}/src/task_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_safe.h"
        "${CMAKE_SOURCE_DIR}/src/sync.h"
        "${CMAKE_SOURCE_DIR}/src/round_robin.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream_recorder.h"
        "${CMAKE_SOURCE_DIR}/src/stream_recorder.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream_stats.h"
        "${CMAKE_SOURCE_DIR}/src/stream_stats.cpp"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.h"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.c"
        ${PLATFORM_TARGET_FILES})

if(NOT POLARIS_ASSETS_DIR_DEF)
    set(POLARIS_ASSETS_DIR_DEF "${POLARIS_ASSETS_DIR}")
endif()
list(APPEND POLARIS_DEFINITIONS POLARIS_ASSETS_DIR="${POLARIS_ASSETS_DIR_DEF}")

list(APPEND POLARIS_DEFINITIONS POLARIS_TRAY=${POLARIS_TRAY})

# Publisher metadata
list(APPEND POLARIS_DEFINITIONS POLARIS_PUBLISHER_NAME="${POLARIS_PUBLISHER_NAME}")
list(APPEND POLARIS_DEFINITIONS POLARIS_PUBLISHER_WEBSITE="${POLARIS_PUBLISHER_WEBSITE}")
list(APPEND POLARIS_DEFINITIONS POLARIS_PUBLISHER_ISSUE_URL="${POLARIS_PUBLISHER_ISSUE_URL}")

include_directories(BEFORE "${CMAKE_SOURCE_DIR}")

include_directories(
        BEFORE
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        ${FFMPEG_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}  # has to be the last, or we get runtime error on macOS ffmpeg encoder
)

list(APPEND POLARIS_EXTERNAL_LIBRARIES
        ${MINIUPNP_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        enet
        libdisplaydevice::display_device
        nlohmann_json::nlohmann_json
        opus
        ${FFMPEG_LIBRARIES}
        ${Boost_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})
