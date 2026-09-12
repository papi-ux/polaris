cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/dependencies/prepared_ffmpeg.cmake")

function(assert_equal actual expected message)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${message}: expected [${expected}], got [${actual}]")
  endif()
endfunction()

function(assert_exists path message)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${message}: missing [${path}]")
  endif()
endfunction()

function(assert_contains haystack needle message)
  string(FIND "${haystack}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "${message}: [${needle}] is not in [${haystack}]")
  endif()
endfunction()

function(write_prepared_tree tree_dir)
  file(MAKE_DIRECTORY "${tree_dir}/include" "${tree_dir}/lib")
  foreach(library IN ITEMS libavcodec.a libswscale.a libavutil.a libcbs.a)
    file(WRITE "${tree_dir}/lib/${library}" "")
  endforeach()
endfunction()

function(write_ffnvcodec_header tree_dir major minor)
  file(WRITE "${tree_dir}/include/ffnvcodec/nvEncodeAPI.h"
      "#define NVENCAPI_MAJOR_VERSION ${major}\n"
      "#define NVENCAPI_MINOR_VERSION ${minor}\n")
endfunction()

set(test_root "${CMAKE_CURRENT_LIST_DIR}/../../build/cmake-prepared-ffmpeg-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY
  "${test_root}/release-source/ffmpeg/include"
  "${test_root}/release-source/ffmpeg/lib"
  "${test_root}/download-cache"
  "${test_root}/release"
)

foreach(library IN ITEMS libavcodec.a libswscale.a libavutil.a libcbs.a)
  file(WRITE "${test_root}/release-source/ffmpeg/lib/${library}" "")
endforeach()
file(WRITE "${test_root}/release-source/ffmpeg/include/.keep" "")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar czf "${test_root}/release/Linux-x86_64-ffmpeg.tar.gz" ffmpeg
  WORKING_DIRECTORY "${test_root}/release-source"
  COMMAND_ERROR_IS_FATAL ANY
)
file(SHA256 "${test_root}/release/Linux-x86_64-ffmpeg.tar.gz" test_archive_sha256)

polaris_prepared_ffmpeg_asset_name(asset_name "Linux" "x86_64")
assert_equal("${asset_name}" "Linux-x86_64-ffmpeg.tar.gz" "Linux x86_64 asset name")

set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "test-release")
set(POLARIS_PREPARED_FFMPEG_BASE_URL "file://${test_root}/release")
set(POLARIS_PREPARED_FFMPEG_CACHE_DIR "${test_root}/download-cache")
set(POLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64 "${test_archive_sha256}")
set(POLARIS_DOWNLOAD_PREPARED_FFMPEG ON)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
unset(FFMPEG_PREPARED_BINARIES)

polaris_resolve_prepared_ffmpeg(resolved_ffmpeg)
set(expected_ffmpeg "${test_root}/download-cache/test-release/Linux-x86_64/ffmpeg")
assert_equal("${resolved_ffmpeg}" "${expected_ffmpeg}" "Resolved FFmpeg directory")
assert_exists("${resolved_ffmpeg}/lib/libavcodec.a" "Extracted libavcodec")
assert_exists("${resolved_ffmpeg}/include/.keep" "Extracted include directory")

file(MAKE_DIRECTORY
  "${test_root}/override/ffmpeg/include"
  "${test_root}/override/ffmpeg/lib"
)
foreach(library IN ITEMS libavcodec.a libswscale.a libavutil.a libcbs.a)
  file(WRITE "${test_root}/override/ffmpeg/lib/${library}" "")
endforeach()
file(WRITE "${test_root}/override/ffmpeg/include/.keep" "")

set(FFMPEG_PREPARED_BINARIES "${test_root}/override/ffmpeg")
set(POLARIS_DOWNLOAD_PREPARED_FFMPEG OFF)
polaris_resolve_prepared_ffmpeg(override_ffmpeg)
assert_equal("${override_ffmpeg}" "${FFMPEG_PREPARED_BINARIES}" "Explicit FFmpeg override")

# The pinned archive decides the minimum NVIDIA driver every Polaris host needs for hardware
# encoding, because libavcodec compiles that requirement in from its ffnvcodec headers. A bump
# that raises it costs every host below that driver its encoder with nothing in the console,
# which is what #650 was. These cases keep that from happening twice.

assert_equal("${POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION}" "13.0"
  "Supported NVENC API ceiling. Raising this drops every driver below what it requires, so it is pinned deliberately")

set(floor_root "${test_root}/nvenc-floor")

write_prepared_tree("${floor_root}/at-ceiling")
write_ffnvcodec_header("${floor_root}/at-ceiling" 13 0)
polaris_prepared_ffmpeg_nvenc_api_version(at_ceiling_version "${floor_root}/at-ceiling")
assert_equal("${at_ceiling_version}" "13.0" "Bundled NVENC API version")
polaris_prepared_ffmpeg_nvenc_floor_violation(at_ceiling_violation "${floor_root}/at-ceiling")
assert_equal("${at_ceiling_violation}" "" "An archive at the ceiling is accepted")

write_prepared_tree("${floor_root}/below-ceiling")
write_ffnvcodec_header("${floor_root}/below-ceiling" 12 0)
polaris_prepared_ffmpeg_nvenc_floor_violation(below_violation "${floor_root}/below-ceiling")
assert_equal("${below_violation}" "" "An archive below the ceiling is accepted")

# No ffnvcodec headers at all is how the Darwin and FreeBSD archives ship, and says nothing
# about NVENC either way.
write_prepared_tree("${floor_root}/no-nvenc")
polaris_prepared_ffmpeg_nvenc_api_version(no_nvenc_version "${floor_root}/no-nvenc")
assert_equal("${no_nvenc_version}" "" "An archive without ffnvcodec headers reports no version")
polaris_prepared_ffmpeg_nvenc_floor_violation(no_nvenc_violation "${floor_root}/no-nvenc")
assert_equal("${no_nvenc_violation}" "" "An archive without ffnvcodec headers is accepted")

write_prepared_tree("${floor_root}/above-ceiling")
write_ffnvcodec_header("${floor_root}/above-ceiling" 13 1)
polaris_prepared_ffmpeg_nvenc_api_version(above_version "${floor_root}/above-ceiling")
assert_equal("${above_version}" "13.1" "Bundled NVENC API version")
polaris_prepared_ffmpeg_nvenc_floor_violation(above_violation "${floor_root}/above-ceiling")
assert_contains("${above_violation}" "NVENC API 13.1" "The refusal names the version found")
assert_contains("${above_violation}" "13.0 Polaris supports" "The refusal names the version supported")

# And prove the guard is wired into the validator every resolve path runs, not just reachable
# as a helper.
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-Dprepared_dir=${floor_root}/above-ceiling"
          -P "${CMAKE_CURRENT_LIST_DIR}/prepared_ffmpeg_floor_guard_case.cmake"
  RESULT_VARIABLE guard_result
  OUTPUT_VARIABLE guard_stdout
  ERROR_VARIABLE guard_stderr
)
if(guard_result EQUAL 0)
  message(FATAL_ERROR "An archive above the ceiling was accepted by polaris_validate_prepared_ffmpeg_dir")
endif()
assert_contains("${guard_stderr}" "NVENC API 13.1" "The validator refusal names the version found")

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-Dprepared_dir=${floor_root}/at-ceiling"
          -P "${CMAKE_CURRENT_LIST_DIR}/prepared_ffmpeg_floor_guard_case.cmake"
  RESULT_VARIABLE accepted_result
  OUTPUT_VARIABLE accepted_stdout
  ERROR_VARIABLE accepted_stderr
)
assert_equal("${accepted_result}" "0" "An archive at the ceiling passes the validator")

message(STATUS "prepared FFmpeg cmake contract: all cases passed")
