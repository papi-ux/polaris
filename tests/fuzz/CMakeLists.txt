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
