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

# Granite is built against volk, which declares every Vulkan entry point as a function pointer
# variable, while Polaris uses the ordinary prototypes. Both are correct in their own translation
# units, and link time optimisation merges them and refuses: "redeclared as variable". Turning
# interprocedural optimisation off for the vendored trees keeps their objects real object code, so
# nothing is merged and both spellings survive. The codec is Vulkan compute either way; the loss is
# inlining across a boundary Polaris does not call across.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Granite" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave" EXCLUDE_FROM_ALL)

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
add_library(polaris_pyrowave STATIC
        "${CMAKE_SOURCE_DIR}/third-party/pyrowave/pyrowave_c.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/Granite/video/scaler.cpp")
target_include_directories(polaris_pyrowave
        PUBLIC "${CMAKE_SOURCE_DIR}/third-party/pyrowave"
        PRIVATE
        "${CMAKE_SOURCE_DIR}/third-party/pyrowave/shaders"
        "${CMAKE_SOURCE_DIR}/third-party/Granite/video")
target_link_libraries(polaris_pyrowave PRIVATE pyrowave granite-vulkan granite-math)
set_target_properties(polaris_pyrowave PROPERTIES POSITION_INDEPENDENT_CODE ON)
