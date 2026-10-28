cmake_minimum_required(VERSION 3.22)
project(PolarisConfigValidatorFuzz LANGUAGES NONE)

# Standalone only: the product and ordinary test projects do not include this directory.
option(POLARIS_BUILD_PARSER_FUZZERS "Build the opt-in parser libFuzzer targets" OFF)
if(NOT POLARIS_BUILD_PARSER_FUZZERS)
    message(STATUS "Polaris config validator fuzzer disabled; no compiler or dependencies requested")
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING
        OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "This harness requires native Linux x86_64; other fuzz platforms are not qualified")
endif()
if(NOT CMAKE_CXX_COMPILER AND "$ENV{CXX}" STREQUAL "")
    message(FATAL_ERROR "Select an installed LLVM Clang C++ compiler explicitly, e.g. -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18")
endif()
enable_language(CXX)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
    message(FATAL_ERROR "This harness requires LLVM Clang 18 or newer, its matching compiler-rt libFuzzer/ASan/UBSan runtimes, and a C++20 standard library")
endif()

# Require an already installed package. Do not include the product's FetchContent fallback.
find_package(nlohmann_json 3.11 QUIET CONFIG)
if(NOT nlohmann_json_FOUND OR NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "Missing local nlohmann_json >=3.11 package; supply its installed nlohmann_json_DIR or CMAKE_PREFIX_PATH (no download is attempted)")
endif()

# This future opt-in configure checks compile/link availability only, never runtime qualification.
include(CheckCXXSourceCompiles)
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
set(CMAKE_REQUIRED_FLAGS "-std=c++20 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer,address,undefined")
check_cxx_source_compiles([=[
    #include <cstddef>
    #include <cstdint>
    extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *, std::size_t) { return 0; }
]=] POLARIS_CONFIG_FUZZ_RUNTIME_LINKS)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT POLARIS_CONFIG_FUZZ_RUNTIME_LINKS)
    message(FATAL_ERROR "Cannot link the libFuzzer entry point with ASan/UBSan; use the selected Clang's matching installed compiler-rt runtimes and inspect the CMake configure log")
endif()

get_filename_component(POLARIS_FUZZ_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)
foreach(required_file IN ITEMS confighttp_validation.cpp confighttp_validation.h app_launch_as.h)
    if(NOT EXISTS "${POLARIS_FUZZ_SOURCE_ROOT}/src/${required_file}")
        message(FATAL_ERROR "Missing production source: ${POLARIS_FUZZ_SOURCE_ROOT}/src/${required_file}")
    endif()
endforeach()

add_executable(fuzz_confighttp_json
    fuzz_confighttp_json.cpp
    "${POLARIS_FUZZ_SOURCE_ROOT}/src/confighttp_validation.cpp")
target_include_directories(fuzz_confighttp_json PRIVATE "${POLARIS_FUZZ_SOURCE_ROOT}/src")
target_link_libraries(fuzz_confighttp_json PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(fuzz_confighttp_json PRIVATE cxx_std_20)
set_target_properties(fuzz_confighttp_json PROPERTIES
    CXX_EXTENSIONS OFF
    INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(fuzz_confighttp_json PRIVATE
    -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
    -fsanitize=fuzzer-no-link,address,undefined)
target_link_options(fuzz_confighttp_json PRIVATE
    -fno-sanitize-recover=all -fsanitize=fuzzer,address,undefined)

# Only the decrypted selective-FEC payload parser; no socket/session or tracker.
if(NOT EXISTS "${POLARIS_FUZZ_SOURCE_ROOT}/src/stream_fec_status.h")
    message(FATAL_ERROR "Missing production source: ${POLARIS_FUZZ_SOURCE_ROOT}/src/stream_fec_status.h")
endif()
add_executable(fuzz_control_fec fuzz_control_fec.cpp)
target_include_directories(fuzz_control_fec PRIVATE "${POLARIS_FUZZ_SOURCE_ROOT}/src")
target_compile_features(fuzz_control_fec PRIVATE cxx_std_20)
set_target_properties(fuzz_control_fec PROPERTIES
    CXX_EXTENSIONS OFF
    INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(fuzz_control_fec PRIVATE
    -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
    -fsanitize=fuzzer-no-link,address,undefined)
target_link_options(fuzz_control_fec PRIVATE
    -fno-sanitize-recover=all -fsanitize=fuzzer,address,undefined)

# Parse the real raw query and individual client launch fields, without a server.
foreach(required_file IN ITEMS src/nvhttp_query.cpp src/nvhttp_query.h src/utility.h)
    if(NOT EXISTS "${POLARIS_FUZZ_SOURCE_ROOT}/${required_file}")
        message(FATAL_ERROR "Missing production source: ${POLARIS_FUZZ_SOURCE_ROOT}/${required_file}")
    endif()
endforeach()
# The pinned SimpleWeb headers come from a gitlink that a plain checkout leaves empty.
# Require them only when this target builds, so the other targets' configure is unchanged.
add_custom_target(fuzz_nvhttp_launch_query_headers
    COMMAND test -f "${POLARIS_FUZZ_SOURCE_ROOT}/third-party/Simple-Web-Server/utility.hpp"
    COMMAND test -f "${POLARIS_FUZZ_SOURCE_ROOT}/third-party/Simple-Web-Server/status_code.hpp"
    COMMENT "Requiring materialized pinned third-party/Simple-Web-Server headers"
    VERBATIM)
add_executable(fuzz_nvhttp_launch_query
    fuzz_nvhttp_launch_query.cpp
    "${POLARIS_FUZZ_SOURCE_ROOT}/src/nvhttp_query.cpp")
add_dependencies(fuzz_nvhttp_launch_query fuzz_nvhttp_launch_query_headers)
target_include_directories(fuzz_nvhttp_launch_query PRIVATE
    "${POLARIS_FUZZ_SOURCE_ROOT}/src"
    "${POLARIS_FUZZ_SOURCE_ROOT}/third-party")
target_link_libraries(fuzz_nvhttp_launch_query PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(fuzz_nvhttp_launch_query PRIVATE cxx_std_20)
set_target_properties(fuzz_nvhttp_launch_query PROPERTIES
    CXX_EXTENSIONS OFF
    INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(fuzz_nvhttp_launch_query PRIVATE
    -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
    -fsanitize=fuzzer-no-link,address,undefined)
target_link_options(fuzz_nvhttp_launch_query PRIVATE
    -fno-sanitize-recover=all -fsanitize=fuzzer,address,undefined)

# Parse raw request bytes with the product's adapted parser copy, then decode Content-length.
foreach(required_file IN ITEMS src/rtsp_content_length.cpp src/rtsp_content_length.h src/utility.h
        cmake/dependencies/rtsp_parser.cmake)
    if(NOT EXISTS "${POLARIS_FUZZ_SOURCE_ROOT}/${required_file}")
        message(FATAL_ERROR "Missing production source: ${POLARIS_FUZZ_SOURCE_ROOT}/${required_file}")
    endif()
endforeach()
# The pinned moonlight-common-c inputs come from a gitlink that a plain checkout leaves empty.
# Only a materialized gitlink enables C and prepares the copy, so other targets' configure is
# unchanged; the product's hash pins and ownership adaptation then apply exactly as in its build.
set(POLARIS_FUZZ_RTSP_PARSER_INPUT "${POLARIS_FUZZ_SOURCE_ROOT}/third-party/moonlight-common-c/src")
if(NOT EXISTS "${POLARIS_FUZZ_RTSP_PARSER_INPUT}/RtspParser.c")
    add_custom_target(fuzz_rtsp_message
        COMMAND "${CMAKE_COMMAND}" -E echo
            "Materialize the pinned third-party/moonlight-common-c gitlink, then reconfigure"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
else()
    if(NOT CMAKE_C_COMPILER AND "$ENV{CC}" STREQUAL "")
        # Use the C driver installed beside the selected Clang C++ compiler.
        get_filename_component(POLARIS_FUZZ_CXX_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(POLARIS_FUZZ_CXX_NAME "${CMAKE_CXX_COMPILER}" NAME)
        string(REGEX REPLACE "^clang\\+\\+" "clang" POLARIS_FUZZ_C_NAME "${POLARIS_FUZZ_CXX_NAME}")
        if(POLARIS_FUZZ_C_NAME STREQUAL POLARIS_FUZZ_CXX_NAME
                OR NOT EXISTS "${POLARIS_FUZZ_CXX_DIR}/${POLARIS_FUZZ_C_NAME}")
            message(FATAL_ERROR "Select the matching LLVM Clang C compiler explicitly, e.g. -DCMAKE_C_COMPILER=/usr/bin/clang-18")
        endif()
        set(CMAKE_C_COMPILER "${POLARIS_FUZZ_CXX_DIR}/${POLARIS_FUZZ_C_NAME}")
    endif()
    enable_language(C)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
            OR NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL CMAKE_CXX_COMPILER_VERSION)
        message(FATAL_ERROR "The adapted RTSP parser must use the same LLVM Clang version as C++ (${CMAKE_CXX_COMPILER_VERSION}) and its runtimes")
    endif()
    include("${POLARIS_FUZZ_SOURCE_ROOT}/cmake/dependencies/rtsp_parser.cmake")
    set(POLARIS_FUZZ_RTSP_PARSER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/moonlight-common-c")
    polaris_prepare_rtsp_parser("${POLARIS_FUZZ_RTSP_PARSER_INPUT}" "${POLARIS_FUZZ_RTSP_PARSER_OUTPUT}")
    add_executable(fuzz_rtsp_message
        fuzz_rtsp_message.cpp
        "${POLARIS_FUZZ_SOURCE_ROOT}/src/rtsp_content_length.cpp"
        "${POLARIS_FUZZ_RTSP_PARSER_OUTPUT}/RtspParser.c")
    target_include_directories(fuzz_rtsp_message PRIVATE
        "${POLARIS_FUZZ_SOURCE_ROOT}/src"
        "${POLARIS_FUZZ_SOURCE_ROOT}/third-party")
    target_link_libraries(fuzz_rtsp_message PRIVATE nlohmann_json::nlohmann_json)
    target_compile_features(fuzz_rtsp_message PRIVATE cxx_std_20)
    set_target_properties(fuzz_rtsp_message PROPERTIES
        CXX_EXTENSIONS OFF
        INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_compile_options(fuzz_rtsp_message PRIVATE
        -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
        -fsanitize=fuzzer-no-link,address,undefined)
    target_link_options(fuzz_rtsp_message PRIVATE
        -fno-sanitize-recover=all -fsanitize=fuzzer,address,undefined)
endif()
