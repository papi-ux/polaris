cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_polaris)

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()
option(BUILD_FULL_TESTS "Build the full Polaris test suite" OFF)

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
if(BUILD_TEST_COVERAGE)
    set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
    set(CMAKE_C_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
endif()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

# modify POLARIS_DEFINITIONS for test-only paths
if (WIN32)
    list(APPEND
            POLARIS_DEFINITIONS POLARIS_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND POLARIS_DEFINITIONS POLARIS_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed

# this indicates we're building tests in case Polaris needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS POLARIS_TESTS)
list(APPEND TEST_DEFINITIONS POLARIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
list(APPEND TEST_DEFINITIONS POLARIS_TEST_BIN_DIR="${CMAKE_CURRENT_BINARY_DIR}")

if(NOT WIN32)
    find_package(Udev 255)  # we need 255+ for udevadm verify
    message(STATUS "UDEV_FOUND: ${UDEV_FOUND}")
    if(UDEV_FOUND)
        list(APPEND TEST_DEFINITIONS UDEVADM_EXECUTABLE="${UDEVADM_EXECUTABLE}")
    endif()
endif()

set(POLARIS_SOURCES
        ${POLARIS_TARGET_FILES})

# remove main.cpp from the list of sources
list(REMOVE_ITEM POLARIS_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

set(TEST_MAIN_SOURCE
    ${CMAKE_SOURCE_DIR}/tests/tests_main.cpp
)

set(TEST_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/confighttp_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/browser_stream_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/file_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/game_library_scanner.cpp
    ${CMAKE_SOURCE_DIR}/src/globals.cpp
    ${CMAKE_SOURCE_DIR}/src/logging.cpp
    ${CMAKE_SOURCE_DIR}/src/rswrapper.c
)

set(TEST_AUDIT_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/file_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/globals.cpp
    ${CMAKE_SOURCE_DIR}/src/logging.cpp
)

set(TEST_FAST_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_confighttp_validation.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_browser_stream_protocol.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_cursor_visibility.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_file_handler.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_game_library_scanner.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_logging.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_rswrapper.cpp
)

if (NOT WIN32 AND NOT APPLE)
    list(APPEND TEST_FAST_SOURCES
        ${CMAKE_SOURCE_DIR}/tests/unit/test_wlgrab_pixel_copy.cpp
    )
endif()

set(TEST_AUDIT_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/integration/test_config_consistency.cpp
    ${CMAKE_SOURCE_DIR}/tests/integration/test_locale_consistency.cpp
)

set(TEST_PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/integration/test_external_commands.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_common.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_cage_display_router.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_cuda_gl_encode_device.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_session_manager.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_virtual_display.cpp
)

set(TEST_INPUT_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_mouse.cpp
)

set(TEST_CONFIG_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_device_db.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_display_device.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_entry_handler.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_process_migration.cpp
)

set(TEST_PAIRING_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_http_pairing.cpp
)

set(TEST_HTTP_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_httpcommon.cpp
)

set(TEST_NETWORK_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_network.cpp
)

set(TEST_AUDIO_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_audio.cpp
)

set(TEST_STREAM_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_adaptive_bitrate.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_ai_optimizer.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_stream.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/test_stream_stats.cpp
)

set(TEST_VIDEO_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_video.cpp
)

# Copy files needed for config consistency tests to build directory
# This ensures both CLI and CLion can access the same files relative to the test executable
# Using configure_file ensures files are copied when they change between builds
set(INTEGRATION_TEST_FILES
    "src/config.cpp"
    "src_assets/common/assets/web/views/ConfigView.vue"
    "docs/configuration.md"
    "src_assets/common/assets/web/public/assets/locale/en.json"
    "src_assets/common/assets/web/configs/tabs/General.vue"
    "src_assets/linux/misc/60-polaris.rules"
)

foreach(file ${INTEGRATION_TEST_FILES})
    configure_file(
        "${CMAKE_SOURCE_DIR}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/${file}"
        COPYONLY
    )
endforeach()

# Copy all locale files for locale consistency tests
# Use a custom command to properly handle both adding and removing files
set(LOCALE_SRC_DIR "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/assets/locale")
set(LOCALE_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/src_assets/common/assets/web/public/assets/locale")
add_custom_target(sync_locale_files ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOCALE_SRC_DIR}" "${LOCALE_DST_DIR}"
    COMMENT "Synchronizing locale files for tests"
    VERBATIM
)

function(apply_polaris_test_target_settings target_name)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23)
    set_target_properties(${target_name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    foreach(dep ${POLARIS_TARGET_DEPENDENCIES})
        add_dependencies(${target_name} ${dep})
    endforeach()

    add_dependencies(${target_name} sync_locale_files)
    target_compile_definitions(${target_name} PUBLIC ${POLARIS_DEFINITIONS} ${TEST_DEFINITIONS})
    target_compile_options(${target_name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${POLARIS_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${POLARIS_COMPILE_OPTIONS_CUDA};-std=c++17>)
endfunction()

function(configure_polaris_test_object_library target_name)
    add_library(${target_name} OBJECT ${ARGN})
    apply_polaris_test_target_settings(${target_name})
    target_link_libraries(${target_name}
            PRIVATE
            ${POLARIS_EXTERNAL_LIBRARIES}
            ${PLATFORM_LIBRARIES})
endfunction()

function(configure_polaris_test_target target_name)
    add_executable(${target_name} ${ARGN})
    apply_polaris_test_target_settings(${target_name})
    target_link_libraries(${target_name}
            ${POLARIS_EXTERNAL_LIBRARIES}
            gtest
            ${PLATFORM_LIBRARIES})
    target_link_options(${target_name} PRIVATE)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    if (WIN32)
        set_target_properties(${target_name} PROPERTIES LINK_SEARCH_START_STATIC 1)
    endif ()
endfunction()

function(configure_polaris_full_test_target target_name)
    configure_polaris_test_target(${target_name}
        $<TARGET_OBJECTS:test_polaris_main_objects>
        $<TARGET_OBJECTS:test_polaris_runtime_objects>
        ${ARGN}
    )
endfunction()

configure_polaris_test_object_library(test_polaris_main_objects
    ${TEST_MAIN_SOURCE}
)

configure_polaris_test_target(${PROJECT_NAME}
    $<TARGET_OBJECTS:test_polaris_main_objects>
    ${TEST_SUPPORT_SOURCES}
    ${TEST_FAST_SOURCES}
)

if (BUILD_FULL_TESTS)
    configure_polaris_test_object_library(test_polaris_runtime_objects
        ${POLARIS_SOURCES}
    )

    configure_polaris_test_target(test_polaris_audit
        $<TARGET_OBJECTS:test_polaris_main_objects>
        ${TEST_AUDIT_SUPPORT_SOURCES}
        ${TEST_AUDIT_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_platform
        ${TEST_PLATFORM_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_input
        ${TEST_INPUT_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_config
        ${TEST_CONFIG_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_pairing
        ${TEST_PAIRING_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_http
        ${TEST_HTTP_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_network
        ${TEST_NETWORK_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_audio
        ${TEST_AUDIO_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_stream
        ${TEST_STREAM_SOURCES}
    )

    configure_polaris_full_test_target(test_polaris_video
        ${TEST_VIDEO_SOURCES}
    )

    # Keep the historical target name as a build aggregate while the runtime coverage
    # lives in smaller executables that are easier to run and debug independently.
    add_custom_target(test_polaris_full
        DEPENDS
            test_polaris_audit
            test_polaris_platform
            test_polaris_input
            test_polaris_config
            test_polaris_pairing
            test_polaris_http
            test_polaris_network
            test_polaris_audio
            test_polaris_stream
            test_polaris_video
    )
endif ()
