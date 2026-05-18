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
