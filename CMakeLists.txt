cmake_minimum_required(VERSION 3.20)
# `CMAKE_CUDA_ARCHITECTURES` requires 3.18
# `set_source_files_properties` requires 3.18
# `cmake_path(CONVERT ... TO_NATIVE_PATH_LIST ...)` requires 3.20
# todo - set this conditionally

# Packaging escape hatch for the GCC>=15 ICEs quarantined in
# cmake/targets/common.cmake: Clang does not hit them, so a packager on an
# affected distro can opt out of GCC entirely instead of waiting on upstream
# GCC fixes. Only takes effect when nothing else already chose a compiler,
# and must run before project() - that is where CMake locks in the compiler.
option(POLARIS_PREFER_CLANG "Use Clang instead of GCC when no compiler is otherwise specified" OFF)
if(POLARIS_PREFER_CLANG AND NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER
        AND NOT DEFINED ENV{CC} AND NOT DEFINED ENV{CXX})
    find_program(POLARIS_CLANG_C_COMPILER NAMES clang)
    find_program(POLARIS_CLANG_CXX_COMPILER NAMES clang++)
    if(NOT POLARIS_CLANG_C_COMPILER OR NOT POLARIS_CLANG_CXX_COMPILER)
        message(FATAL_ERROR "POLARIS_PREFER_CLANG=ON but clang/clang++ was not found on PATH")
    endif()
    set(CMAKE_C_COMPILER "${POLARIS_CLANG_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${POLARIS_CLANG_CXX_COMPILER}" CACHE FILEPATH "C++ compiler" FORCE)
endif()

project(Polaris VERSION 1.4.1
        DESCRIPTION "Self-hosted game stream host for Moonlight"
        HOMEPAGE_URL "https://github.com/papi-ux/polaris")

include(CTest)

set(PROJECT_LICENSE "GPL-3.0-only")

set(PROJECT_FQDN "dev.polaris-stream.app.Polaris")

set(PROJECT_BRIEF_DESCRIPTION "GameStream host for Moonlight")  # must be <= 35 characters

set(PROJECT_LONG_DESCRIPTION "Polaris offers low latency, cloud gaming server capabilities with support for AMD, Intel, \
and Nvidia GPUs for hardware encoding. Software encoding is also available. You can connect to Polaris from any \
Moonlight client on a variety of devices. A web UI is provided to allow configuration, and client pairing, from \
your favorite web browser. Pair from the local server or any mobile device.")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Enable Link-Time Optimization for Release builds
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
if(IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Enabling Link-Time Optimization (LTO)")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
else()
    if(NOT IPO_SUPPORTED)
        message(STATUS "LTO not supported: ${IPO_OUTPUT}")
    endif()
endif()

# set the module path, used for includes
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set version info for this build
include(${CMAKE_MODULE_PATH}/prep/build_version.cmake)

# cmake build flags
include(${CMAKE_MODULE_PATH}/prep/options.cmake)

# initial prep
include(${CMAKE_MODULE_PATH}/prep/init.cmake)

# configure special package files, such as polaris.desktop and Portfile
include(${CMAKE_MODULE_PATH}/prep/special_package_configuration.cmake)

# Exit early if END_BUILD is ON, i.e. when only generating package manifests
if(${END_BUILD})
    return()
endif()

# project constants
include(${CMAKE_MODULE_PATH}/prep/constants.cmake)

# load macros
include(${CMAKE_MODULE_PATH}/macros/common.cmake)

# load dependencies
include(${CMAKE_MODULE_PATH}/dependencies/common.cmake)

# setup compile definitions
include(${CMAKE_MODULE_PATH}/compile_definitions/common.cmake)

# target definitions
include(${CMAKE_MODULE_PATH}/targets/common.cmake)

# packaging
include(${CMAKE_MODULE_PATH}/packaging/common.cmake)
