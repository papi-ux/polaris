# PyroWave, an intra-only wavelet codec that runs as plain Vulkan compute.
#
# On by default, so a host can serve it to a client that asks. Nothing else changes for a host that
# never gets asked: only a client carrying this codec's payload number can negotiate it, no Moonlight
# client does, and the host refuses it where it cannot run.
#
# The cost of being on is real and worth naming. It brings Granite in behind it, which is a large
# dependency tree to fetch and compile, and it wants a wired link at a couple of hundred megabits to
# be worth choosing. Turning it off with -DPOLARIS_ENABLE_PYROWAVE=OFF builds exactly as this tree
# did before the codec existed.

if(NOT POLARIS_ENABLE_PYROWAVE)
    return()
endif()

foreach(module pyrowave Granite)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third-party/${module}/CMakeLists.txt")
        message(FATAL_ERROR
                "POLARIS_ENABLE_PYROWAVE is on but third-party/${module} is empty. "
                "Run: git submodule update --init --recursive third-party/pyrowave third-party/Granite")
    endif()
endforeach()

# PyroWave adds Granite itself only when it is the top level project. Inside Polaris it is not, so
# Polaris adds Granite first with the option set PyroWave's own shipping build uses, and PyroWave
# then finds the granite-vulkan target already there and links it.
#
# Keeping these in step with upstream is the price of building it in tree. A mismatch surfaces as a
# link error rather than as a subtly different codec.
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_FFMPEG OFF CACHE BOOL "" FORCE)
set(GRANITE_FFMPEG_VULKAN OFF CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)

# Granite's older minimum CMake version otherwise ignores the project's IPO
# setting. Keep that policy local to these dependencies and restore the caller.
set(_polaris_previous_ipo_policy "${CMAKE_POLICY_DEFAULT_CMP0069}")
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Granite" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave" EXCLUDE_FROM_ALL)
if(_polaris_previous_ipo_policy STREQUAL "")
    unset(CMAKE_POLICY_DEFAULT_CMP0069)
else()
    set(CMAKE_POLICY_DEFAULT_CMP0069 "${_polaris_previous_ipo_policy}")
endif()
unset(_polaris_previous_ipo_policy)

# Granite builds volk itself instead of using volk's CMake options. Apply volk's
# supported C++ namespace to its shared header interface and implementation. This
# keeps pointer globals distinct from the host's ordinary Vulkan loader functions
# under LTO without changing either side's dispatch or initialization ownership.
target_compile_definitions(granite-volk-headers INTERFACE VOLK_NAMESPACE)
set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/third-party/Granite/third_party/volk/volk.c"
        TARGET_DIRECTORY granite-volk
        PROPERTIES LANGUAGE CXX)

# The C entry points are the ones Polaris can use: they take a VkInstance, VkPhysicalDevice and
# VkDevice somebody else made, which is what this host has once FFmpeg has built a Vulkan device.
# The C++ API wants a Granite device Polaris does not own.
#
# PyroWave defines that target only for its own top level build, so build the same sources here
# rather than teaching upstream about us. Static rather than shared: there is no ABI to hold still
# when the only caller is linked into the same binary, and upstream says its C ABI is not stable
# before 1.0.
# Upstream reaches into Granite as a directory inside its own tree. Polaris keeps Granite as its own
# submodule, because a submodule cannot live inside another one, so these two paths differ from the
# spelling in PyroWave's CMakeLists.
include("${CMAKE_CURRENT_LIST_DIR}/pyrowave_scaler.cmake")
set(POLARIS_PYROWAVE_SCALER_SOURCE_DIR "${CMAKE_BINARY_DIR}/dependencies/pyrowave-scaler-source")
polaris_prepare_pyrowave_scaler(
        "${CMAKE_SOURCE_DIR}/third-party/pyrowave" "${POLARIS_PYROWAVE_SCALER_SOURCE_DIR}")
add_library(polaris_pyrowave STATIC
        "${POLARIS_PYROWAVE_SCALER_SOURCE_DIR}/pyrowave_c.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/Granite/video/scaler.cpp")
target_include_directories(polaris_pyrowave
        PUBLIC "${CMAKE_SOURCE_DIR}/third-party/pyrowave"
        PRIVATE
        "${CMAKE_SOURCE_DIR}/third-party/pyrowave/shaders"
        "${CMAKE_SOURCE_DIR}/third-party/Granite/video")
target_link_libraries(polaris_pyrowave PRIVATE pyrowave granite-vulkan granite-math)
# Upstream's bitrate model, a generated header and nothing to link. Public, so the host's own advice
# (src/pyrowave_advice.cpp) evaluates the same polynomials the codec's author fitted.
target_link_libraries(polaris_pyrowave PUBLIC pyrowave-regression-results)
set_target_properties(polaris_pyrowave PROPERTIES POSITION_INDEPENDENT_CODE ON)
