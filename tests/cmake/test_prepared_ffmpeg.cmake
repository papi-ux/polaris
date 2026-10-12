cmake_minimum_required(VERSION 3.20)

# The module under test. Overridable so the same cases can be run against another revision of
# the module, which is how the warm-tree cases below were shown to fail before the fix for #203.
if(NOT DEFINED PREPARED_FFMPEG_MODULE)
  set(PREPARED_FFMPEG_MODULE "${CMAKE_CURRENT_LIST_DIR}/../../cmake/dependencies/prepared_ffmpeg.cmake")
endif()
include("${PREPARED_FFMPEG_MODULE}")

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

# A warm build tree keeps the release tag and base URL it was first configured with, while the
# hashes are read from the source every time. That is #203 (GitHub #791): a tree configured on
# an older pin downloaded the old archive and failed the new hash check, and overriding only the
# tag still fetched from the cached base URL. The cases below configure real build trees against
# local archives, the way such a tree comes to exist, and assert what the source must do with
# each shape. Before the fix the first of them is a bare HASH mismatch.

function(assert_not_contains haystack needle message)
  string(FIND "${haystack}" "${needle}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR "${message}: [${needle}] is in [${haystack}]")
  endif()
endfunction()

function(assert_not_exists path message)
  if(EXISTS "${path}")
    message(FATAL_ERROR "${message}: present [${path}]")
  endif()
endfunction()

# CMake wraps diagnostics at the terminal width, so captured output is flattened before any
# phrase is looked for in it.
function(flatten out_var text)
  string(REGEX REPLACE "[ \t\r\n]+" " " flat "${text}")
  set("${out_var}" "${flat}" PARENT_SCOPE)
endfunction()

# Paths are normalised because the configures below print them back.
get_filename_component(warm_root "${test_root}/warm" ABSOLUTE)
set(release_root "file://${warm_root}/releases")
# The tag the module pins: the cache default it set when it was included above, before the
# script-level overrides of the earlier cases.
get_property(source_tag CACHE POLARIS_PREPARED_FFMPEG_RELEASE_TAG PROPERTY VALUE)
assert_equal("${source_tag}" "v2026.713.132551"
  "The pinned release tag. Moving it is a deliberate change with its own hash and NVENC review, never a cache fix")

# One local archive per release, each with distinct content so their hashes differ.
function(write_release_archive tag marker out_hash_var)
  set(source "${warm_root}/sources/${tag}/ffmpeg")
  file(MAKE_DIRECTORY "${source}/include" "${source}/lib" "${warm_root}/releases/${tag}")
  foreach(library IN ITEMS libavcodec.a libswscale.a libavutil.a libcbs.a)
    file(WRITE "${source}/lib/${library}" "")
  endforeach()
  file(WRITE "${source}/include/.keep" "${marker}\n")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar czf "${warm_root}/releases/${tag}/Linux-x86_64-ffmpeg.tar.gz" ffmpeg
    WORKING_DIRECTORY "${warm_root}/sources/${tag}"
    COMMAND_ERROR_IS_FATAL ANY
  )
  file(SHA256 "${warm_root}/releases/${tag}/Linux-x86_64-ffmpeg.tar.gz" hash)
  set("${out_hash_var}" "${hash}" PARENT_SCOPE)
endfunction()

write_release_archive("${source_tag}" "current pin" current_sha256)
write_release_archive("v2026.724.203728" "legacy pin" legacy_sha256)
write_release_archive("test-release-b" "next pin" next_sha256)
if(current_sha256 STREQUAL legacy_sha256 OR current_sha256 STREQUAL next_sha256)
  message(FATAL_ERROR "The local release archives must not share a hash")
endif()
# A mirror outside the release root: a base URL that names no release at all.
file(MAKE_DIRECTORY "${warm_root}/mirror")
file(COPY "${warm_root}/releases/${source_tag}/Linux-x86_64-ffmpeg.tar.gz" DESTINATION "${warm_root}/mirror")

# The project under configure. Its legacy phase is the first five lines of the module as every
# Polaris before this fix shipped them, with whatever tag that source pinned; its current phase
# includes the module under test and resolves an archive, as the product build does.
file(MAKE_DIRECTORY "${warm_root}/project")
file(WRITE "${warm_root}/project/CMakeLists.txt" [==[
cmake_minimum_required(VERSION 3.20)
project(prepared_ffmpeg_warm_tree NONE)
if(WARM_PHASE STREQUAL "legacy")
  set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "${WARM_LEGACY_TAG}" CACHE STRING
      "LizardByte/build-deps release tag used for prepared FFmpeg archives")
  set(POLARIS_PREPARED_FFMPEG_BASE_URL
      "${POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL}/${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}"
      CACHE STRING "Base URL for prepared FFmpeg release archives")
  message(STATUS "legacy phase cached ${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}")
else()
  set(CMAKE_SYSTEM_NAME Linux)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  include("${WARM_MODULE}")
  polaris_resolve_prepared_ffmpeg(resolved)
  message(STATUS "WARM_RESOLVED=${resolved}")
endif()
]==])

# Configures one build tree. Output is returned flattened. Extra -D arguments follow.
function(configure_warm_tree tree_dir phase module result_var stdout_var stderr_var)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${warm_root}/project" -B "${tree_dir}"
            "-DWARM_PHASE=${phase}"
            "-DWARM_MODULE=${module}"
            "-DPOLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL=${release_root}"
            ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  flatten(stdout "${stdout}")
  flatten(stderr "${stderr}")
  set("${result_var}" "${result}" PARENT_SCOPE)
  set("${stdout_var}" "${stdout}" PARENT_SCOPE)
  set("${stderr_var}" "${stderr}" PARENT_SCOPE)
endfunction()

function(read_cache_entries tree_dir out_var)
  file(STRINGS "${tree_dir}/CMakeCache.txt" entries REGEX "^POLARIS_PREPARED_FFMPEG_[A-Z_]+:")
  set("${out_var}" "${entries}" PARENT_SCOPE)
endfunction()

# What cmake --fresh does, without depending on the flag.
function(fresh_tree tree_dir)
  file(REMOVE "${tree_dir}/CMakeCache.txt")
  file(REMOVE_RECURSE "${tree_dir}/CMakeFiles")
endfunction()

set(module "${PREPARED_FFMPEG_MODULE}")

# Case: the reporter's tree. Configured on the 1.4.1 to 1.4.7 pin, reconfigured on the current
# source. The tree carries no record of how its tag was chosen, so the source stops before any
# download and says what to do. Before the fix this is a bare HASH mismatch.
set(reporter_tree "${warm_root}/trees/reporter")
configure_warm_tree("${reporter_tree}" legacy "${module}" legacy_result legacy_stdout legacy_stderr
  "-DWARM_LEGACY_TAG=v2026.724.203728")
assert_equal("${legacy_result}" "0" "Legacy configure of the reporter tree: ${legacy_stderr}")
configure_warm_tree("${reporter_tree}" current "${module}" reporter_result reporter_stdout reporter_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
if(reporter_result EQUAL 0)
  message(FATAL_ERROR "A warm tree caching an unaccounted-for release tag must not configure silently")
endif()
assert_not_contains("${reporter_stderr}" "HASH mismatch" "The stop is not a bare hash mismatch")
assert_contains("${reporter_stderr}" "caches prepared FFmpeg release tag v2026.724.203728" "The stop names the cached tag")
assert_contains("${reporter_stderr}" "the source pins ${source_tag}" "The stop names the source tag")
assert_contains("${reporter_stderr}" "cmake --fresh" "The stop names the way out")
assert_contains("${reporter_stderr}" "-DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=v2026.724.203728" "The stop names how to keep the tag deliberately")
assert_not_exists("${reporter_tree}/_deps/prepared-ffmpeg/v2026.724.203728/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the cached tag")
assert_not_exists("${reporter_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the source tag either")
read_cache_entries("${reporter_tree}" reporter_cache)
assert_contains("${reporter_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=v2026.724.203728" "The stop leaves the cached tag alone")
assert_not_contains("${reporter_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG" "A stopped tree records nothing, so it stops again until somebody acts")

# Case: on that same tree a local prepared directory still works, and disabled downloads keep
# their existing refusal, because the stop belongs to the download.
configure_warm_tree("${reporter_tree}" current "${module}" local_result local_stdout local_stderr
  "-DFFMPEG_PREPARED_BINARIES=${test_root}/override/ffmpeg" -DPOLARIS_DOWNLOAD_PREPARED_FFMPEG=OFF)
assert_equal("${local_result}" "0" "A local prepared directory is honoured on a stopped tree: ${local_stderr}")
assert_contains("${local_stdout}" "WARM_RESOLVED=${test_root}/override/ffmpeg" "The local directory is what was resolved")
configure_warm_tree("${reporter_tree}" current "${module}" disabled_result disabled_stdout disabled_stderr
  -UFFMPEG_PREPARED_BINARIES -DPOLARIS_DOWNLOAD_PREPARED_FFMPEG=OFF)
if(disabled_result EQUAL 0)
  message(FATAL_ERROR "Disabled downloads without a local directory must still refuse")
endif()
assert_contains("${disabled_stderr}" "No prepared FFmpeg for Linux-x86_64" "Disabled downloads keep their existing refusal")
assert_not_exists("${reporter_tree}/_deps/prepared-ffmpeg" "Nothing was downloaded with downloads disabled")

# Case: the way out. A fresh tree on the current source resolves the pinned release and records
# the default it was configured with; configured again it is an unchanged warm tree.
fresh_tree("${reporter_tree}")
configure_warm_tree("${reporter_tree}" current "${module}" fresh_result fresh_stdout fresh_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${fresh_result}" "0" "A fresh tree resolves the pinned release: ${fresh_stderr}")
assert_contains("${fresh_stdout}" "Using prepared FFmpeg Linux-x86_64-ffmpeg.tar.gz from ${source_tag}" "The status line names the pinned release")
assert_contains("${fresh_stdout}" "WARM_RESOLVED=${reporter_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The pinned release's archive was resolved")
read_cache_entries("${reporter_tree}" fresh_cache)
assert_contains("${fresh_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The cache holds the pinned tag")
assert_contains("${fresh_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The cached base URL follows the tag")
assert_contains("${fresh_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG:INTERNAL=${source_tag}" "The tree records the source default it was configured with")
configure_warm_tree("${reporter_tree}" current "${module}" again_result again_stdout again_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${again_result}" "0" "An unchanged warm tree reconfigures cleanly: ${again_stderr}")
assert_not_contains("${again_stdout}" "moved from" "An unchanged warm tree moves nothing")
assert_contains("${again_stdout}" "WARM_RESOLVED=${reporter_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The same archive is resolved again")

# Case: the source moves its pin under a tree that recorded the current default. The next source
# is this module with its one declaration moved to test-release-b; the cached tag equals the
# recorded default, so it is not a choice and follows the source, and so does the base URL.
file(READ "${module}" module_text)
string(REPLACE "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"${source_tag}\" CACHE STRING"
  "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"test-release-b\" CACHE STRING" next_module_text "${module_text}")
if(next_module_text STREQUAL module_text)
  message(FATAL_ERROR "The module's one release tag declaration was not found, so a source move cannot be simulated")
endif()
file(MAKE_DIRECTORY "${warm_root}/next-source/cmake/dependencies")
set(next_module "${warm_root}/next-source/cmake/dependencies/prepared_ffmpeg.cmake")
file(WRITE "${next_module}" "${next_module_text}")
set(moving_tree "${warm_root}/trees/moving")
configure_warm_tree("${moving_tree}" current "${module}" moving_first_result moving_first_stdout moving_first_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${moving_first_result}" "0" "A fresh tree on the current source: ${moving_first_stderr}")
configure_warm_tree("${moving_tree}" current "${next_module}" moving_result moving_stdout moving_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${moving_result}" "0" "A tracked warm tree follows a moved pin: ${moving_stderr}")
assert_contains("${moving_stdout}" "release tag moved from ${source_tag} to test-release-b" "The reconfigure says which default moved and to what")
assert_contains("${moving_stdout}" "base URL moved from ${release_root}/${source_tag} to ${release_root}/test-release-b" "The base URL followed the tag")
assert_contains("${moving_stdout}" "Using prepared FFmpeg Linux-x86_64-ffmpeg.tar.gz from test-release-b" "The status line names the moved release")
assert_contains("${moving_stdout}" "WARM_RESOLVED=${moving_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The moved release's archive was resolved")
read_cache_entries("${moving_tree}" moving_cache)
assert_contains("${moving_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b" "The cache now holds the moved tag")
assert_contains("${moving_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/test-release-b" "The cached base URL followed the moved tag")
assert_contains("${moving_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG:INTERNAL=test-release-b" "The tree records the moved default")
configure_warm_tree("${moving_tree}" current "${next_module}" moved_again_result moved_again_stdout moved_again_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${moved_again_result}" "0" "The moved tree reconfigures cleanly: ${moved_again_stderr}")
assert_not_contains("${moved_again_stdout}" "moved from" "Nothing moves twice")

# Case: a tag given on the command line that happens to equal the current default is still a
# choice. When the source later moves its pin, that tree keeps what it was told rather than
# following the default it never asked for.
set(same_default_tree "${warm_root}/trees/same-default")
configure_warm_tree("${same_default_tree}" current "${module}" same_default_result same_default_stdout same_default_stderr
  "-DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=${source_tag}"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${same_default_result}" "0" "An explicit tag equal to the default resolves the pin: ${same_default_stderr}")
assert_contains("${same_default_stdout}" "WARM_RESOLVED=${same_default_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The pinned archive was resolved")
read_cache_entries("${same_default_tree}" same_default_cache)
assert_contains("${same_default_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG:INTERNAL=${source_tag}" "The tree records that the tag was given")
configure_warm_tree("${same_default_tree}" current "${next_module}" same_default_moved_result same_default_moved_stdout same_default_moved_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${same_default_moved_result}" "0" "A chosen tag equal to the old default survives a moved pin: ${same_default_moved_stderr}")
assert_not_contains("${same_default_moved_stdout}" "moved from" "A choice is not migrated because its value equalled the default")
assert_contains("${same_default_moved_stdout}" "${source_tag} is kept as this tree's override, given on its command line" "The reconfigure says the choice is kept")
assert_contains("${same_default_moved_stdout}" "WARM_RESOLVED=${same_default_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The chosen archive is still resolved")
read_cache_entries("${same_default_tree}" same_default_moved_cache)
assert_contains("${same_default_moved_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The chosen tag stays cached")
assert_contains("${same_default_moved_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The base URL stays with the chosen tag")

# Case: a tag given on the command line is a choice, even on a tree with no record, and the
# stale base URL follows it instead of staying on the old release. Before the fix the status
# line named the chosen tag while the download still fetched the cached URL.
set(chosen_tree "${warm_root}/trees/chosen")
configure_warm_tree("${chosen_tree}" legacy "${module}" chosen_legacy_result chosen_legacy_stdout chosen_legacy_stderr
  "-DWARM_LEGACY_TAG=v2026.724.203728")
assert_equal("${chosen_legacy_result}" "0" "Legacy configure of the chosen tree: ${chosen_legacy_stderr}")
configure_warm_tree("${chosen_tree}" current "${module}" chosen_result chosen_stdout chosen_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${chosen_result}" "0" "A tag-only override on a legacy tree resolves that release: ${chosen_stderr}")
assert_contains("${chosen_stdout}" "base URL moved from ${release_root}/v2026.724.203728 to ${release_root}/test-release-b" "The stale base URL followed the chosen tag")
assert_contains("${chosen_stdout}" "Using prepared FFmpeg Linux-x86_64-ffmpeg.tar.gz from test-release-b" "The status line names the chosen release")
assert_contains("${chosen_stdout}" "WARM_RESOLVED=${chosen_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The chosen release's archive was resolved")
assert_not_exists("${chosen_tree}/_deps/prepared-ffmpeg/v2026.724.203728/Linux-x86_64-ffmpeg.tar.gz" "The stale release was not downloaded")
read_cache_entries("${chosen_tree}" chosen_cache)
assert_contains("${chosen_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b" "The chosen tag is cached")
assert_contains("${chosen_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/test-release-b" "The base URL was recomputed for the chosen tag")
# And the choice survives the next configure on the same source: it is not the default, so it stays.
configure_warm_tree("${chosen_tree}" current "${module}" kept_result kept_stdout kept_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${kept_result}" "0" "A chosen tag survives reconfigure: ${kept_stderr}")
assert_contains("${kept_stdout}" "test-release-b is kept as this tree's override" "The reconfigure says the choice is kept")
assert_contains("${kept_stdout}" "WARM_RESOLVED=${chosen_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The chosen release is still resolved")

# Case: a tag given with an explicit cache type (-D...:STRING=) is already typed when this file
# runs, so on a tree with no record it cannot be told from an inherited value. It is stopped with
# the cached value preserved and the untyped form named; given untyped once, it is recorded as a
# choice and kept from then on.
set(typed_tree "${warm_root}/trees/typed")
configure_warm_tree("${typed_tree}" current "${module}" typed_result typed_stdout typed_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(typed_result EQUAL 0)
  message(FATAL_ERROR "A typed tag on a tree with no record must not configure silently")
endif()
assert_not_contains("${typed_stderr}" "HASH mismatch" "The stop is not a bare hash mismatch")
assert_contains("${typed_stderr}" "caches prepared FFmpeg release tag test-release-b" "The stop names the typed tag")
assert_contains("${typed_stderr}" "-DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b" "The stop names the untyped form that records a choice")
assert_not_exists("${typed_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the typed tag")
read_cache_entries("${typed_tree}" typed_cache)
assert_contains("${typed_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b" "The stop preserves the cached value")
assert_not_contains("${typed_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG" "A stopped tree records nothing")
configure_warm_tree("${typed_tree}" current "${module}" untyped_result untyped_stdout untyped_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${untyped_result}" "0" "The untyped form records the choice and resolves it: ${untyped_stderr}")
assert_contains("${untyped_stdout}" "WARM_RESOLVED=${typed_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The chosen release was resolved")
read_cache_entries("${typed_tree}" untyped_cache)
assert_contains("${untyped_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG:INTERNAL=${source_tag}" "The tree now records the source default beside the choice")
configure_warm_tree("${typed_tree}" current "${module}" typed_kept_result typed_kept_stdout typed_kept_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${typed_kept_result}" "0" "The recorded choice survives reconfigure: ${typed_kept_stderr}")
assert_contains("${typed_kept_stdout}" "test-release-b is kept as this tree's override" "The reconfigure says the choice is kept")

# Case: an explicit base URL that names no release, such as a mirror, is kept for the pinned tag.
set(mirror_tree "${warm_root}/trees/mirror")
configure_warm_tree("${mirror_tree}" current "${module}" mirror_result mirror_stdout mirror_stderr
  "-DPOLARIS_PREPARED_FFMPEG_BASE_URL=file://${warm_root}/mirror"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${mirror_result}" "0" "A mirror base URL is honoured: ${mirror_stderr}")
assert_contains("${mirror_stdout}" "WARM_RESOLVED=${mirror_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The pinned tag is resolved from the mirror")
read_cache_entries("${mirror_tree}" mirror_cache)
assert_contains("${mirror_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=file://${warm_root}/mirror" "The mirror URL is kept")

# Case: a base URL given on the command line that happens to equal the derived URL is still a
# choice. When a different tag is given later, the earlier choice is not quietly rewritten to
# follow it: the pair is a mixed override and stops.
set(same_url_tree "${warm_root}/trees/same-url")
configure_warm_tree("${same_url_tree}" current "${module}" same_url_result same_url_stdout same_url_stderr
  "-DPOLARIS_PREPARED_FFMPEG_BASE_URL=${release_root}/${source_tag}"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${same_url_result}" "0" "An explicit base URL equal to the derived one resolves the pin: ${same_url_stderr}")
assert_contains("${same_url_stdout}" "WARM_RESOLVED=${same_url_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The pinned archive was resolved")
read_cache_entries("${same_url_tree}" same_url_cache)
assert_contains("${same_url_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL:INTERNAL=${release_root}/${source_tag}" "The tree records that the base URL was given")
configure_warm_tree("${same_url_tree}" current "${module}" same_url_moved_result same_url_moved_stdout same_url_moved_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(same_url_moved_result EQUAL 0)
  message(FATAL_ERROR "A chosen base URL must not be rewritten to follow a tag given later")
endif()
assert_not_contains("${same_url_moved_stdout}" "base URL moved from" "The chosen base URL is not recomputed")
assert_contains("${same_url_moved_stderr}" "names release ${source_tag} while POLARIS_PREPARED_FFMPEG_RELEASE_TAG is test-release-b" "The stop names both releases")
assert_not_exists("${same_url_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the mixed pair")
read_cache_entries("${same_url_tree}" same_url_moved_cache)
assert_contains("${same_url_moved_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The chosen base URL is preserved")
assert_contains("${same_url_moved_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b" "The given tag is preserved too, for the person to reconcile")

# Case: a tag and a base URL that name different releases is a mixed override. It stops before
# any download rather than letting the status line and the download disagree.
set(mixed_tree "${warm_root}/trees/mixed")
configure_warm_tree("${mixed_tree}" current "${module}" mixed_result mixed_stdout mixed_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_BASE_URL=${release_root}/${source_tag}"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(mixed_result EQUAL 0)
  message(FATAL_ERROR "A tag and a base URL naming different releases must not configure silently")
endif()
assert_contains("${mixed_stderr}" "names release ${source_tag} while POLARIS_PREPARED_FFMPEG_RELEASE_TAG is test-release-b" "The stop names both releases")
assert_not_exists("${mixed_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the mixed override")
assert_not_exists("${mixed_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the mixed override either")

# The pin is declared exactly once, in the form the media provenance collector reads, and the
# module reads its own declaration rather than keeping a second copy of the tag.
file(STRINGS "${module}" tag_declarations REGEX "^set\\(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"[^\"]+\"")
list(LENGTH tag_declarations tag_declaration_count)
assert_equal("${tag_declaration_count}" "1" "Exactly one literal release tag declaration")
assert_contains("${tag_declarations}" "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"${source_tag}\" CACHE STRING" "The declaration carries the pin as a literal")
polaris_prepared_ffmpeg_source_default_release_tag(parsed_default)
assert_equal("${parsed_default}" "${source_tag}" "The module reads its own declaration")
file(STRINGS "${module}" linux_hash_lines REGEX "66512409857d7c11c18875193c098a5131baec060169c8f8e6397387e7a1af7d")
list(LENGTH linux_hash_lines linux_hash_count)
assert_equal("${linux_hash_count}" "1" "The Linux x86_64 hash of the pin is declared exactly once")

# The two decisions are pure functions. Each shape they can see has a case here, including the
# ones a build tree cannot conveniently be put into.

polaris_prepared_ffmpeg_classify_release_tag(fresh_case "${source_tag}" "" "" "" "")
assert_equal("${fresh_case_ACTION}" "fresh" "No cache entry is a fresh tree")
assert_equal("${fresh_case_TAG}" "${source_tag}" "A fresh tree takes the source default")

polaris_prepared_ffmpeg_classify_release_tag(same_case "${source_tag}" "${source_tag}" "STRING" "${source_tag}" "")
assert_equal("${same_case_ACTION}" "unchanged" "A cached tag equal to the source default is unchanged")

polaris_prepared_ffmpeg_classify_release_tag(same_untracked_case "${source_tag}" "${source_tag}" "STRING" "" "")
assert_equal("${same_untracked_case_ACTION}" "unchanged" "A cached tag equal to the source default needs no record")

polaris_prepared_ffmpeg_classify_release_tag(tracked_case "test-release-b" "${source_tag}" "STRING" "${source_tag}" "")
assert_equal("${tracked_case_ACTION}" "migrated" "A cached tag equal to the recorded default moves with the source")
assert_equal("${tracked_case_TAG}" "test-release-b" "It moves to the new source default")
assert_contains("${tracked_case_MESSAGE}" "moved from ${source_tag} to test-release-b" "The move names both tags")

polaris_prepared_ffmpeg_classify_release_tag(choice_case "${source_tag}" "my-pin" "STRING" "${source_tag}" "")
assert_equal("${choice_case_ACTION}" "override-kept" "A cached tag that differs from the recorded default is a choice")
assert_equal("${choice_case_TAG}" "my-pin" "The choice is kept")

polaris_prepared_ffmpeg_classify_release_tag(old_choice_case "${source_tag}" "v2026.724.203728" "STRING" "v2026.516.30821" "")
assert_equal("${old_choice_case_ACTION}" "override-kept" "An earlier pin chosen on a tracked tree is a choice, not a default to move")

polaris_prepared_ffmpeg_classify_release_tag(legacy_case "${source_tag}" "v2026.724.203728" "STRING" "" "")
assert_equal("${legacy_case_ACTION}" "refuse" "An untracked tree caching an earlier pin is stopped, not guessed about")
assert_equal("${legacy_case_TAG}" "v2026.724.203728" "The cached tag is reported as the effective one")
assert_contains("${legacy_case_MESSAGE}" "v2026.724.203728" "The stop names the cached tag")
assert_contains("${legacy_case_MESSAGE}" "${source_tag}" "The stop names the source tag")
assert_contains("${legacy_case_MESSAGE}" "cmake --fresh" "The stop names the way out")

polaris_prepared_ffmpeg_classify_release_tag(typed_case "${source_tag}" "my-pin" "STRING" "" "")
assert_equal("${typed_case_ACTION}" "refuse" "A typed override on an untracked tree is indistinguishable from an inherited value and is stopped")

polaris_prepared_ffmpeg_classify_release_tag(given_case "${source_tag}" "my-pin" "UNINITIALIZED" "" "")
assert_equal("${given_case_ACTION}" "explicit" "A -D on this command line is a choice made now")
assert_equal("${given_case_TAG}" "my-pin" "The given tag is effective")

polaris_prepared_ffmpeg_classify_release_tag(given_tracked_case "${source_tag}" "my-pin" "UNINITIALIZED" "${source_tag}" "")
assert_equal("${given_tracked_case_ACTION}" "explicit" "A -D on a tracked tree is a choice made now")

# The URL helpers read the release root the warm trees were given above.
set(POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL "${release_root}")
polaris_prepared_ffmpeg_classify_release_tag(chosen_same_case "test-release-b" "${source_tag}" "STRING" "${source_tag}" "${source_tag}")
assert_equal("${chosen_same_case_ACTION}" "override-kept" "A recorded choice equal to the old default stays a choice when the default moves")
assert_equal("${chosen_same_case_TAG}" "${source_tag}" "The chosen tag stays effective")
assert_contains("${chosen_same_case_MESSAGE}" "given on its command line" "The message says why it is kept")

polaris_prepared_ffmpeg_classify_release_tag(chosen_current_case "${source_tag}" "${source_tag}" "STRING" "${source_tag}" "${source_tag}")
assert_equal("${chosen_current_case_ACTION}" "unchanged" "A recorded choice equal to the current default needs no message")

polaris_prepared_ffmpeg_classify_release_tag(chosen_stale_case "test-release-b" "${source_tag}" "STRING" "${source_tag}" "my-pin")
assert_equal("${chosen_stale_case_ACTION}" "migrated" "A stale choice record for another value does not stop a recorded default from moving")

polaris_prepared_ffmpeg_default_base_url(derived_url "${source_tag}")
assert_equal("${derived_url}" "${release_root}/${source_tag}" "The default base URL is the release root plus the tag")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_tag "${release_root}/v2026.724.203728/")
assert_equal("${parsed_tag}" "v2026.724.203728" "A default-form base URL names its release, with or without a trailing slash")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_mirror "file://${warm_root}/mirror")
assert_equal("${parsed_mirror}" "" "A URL outside the release root names no release")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_deep "${release_root}/v2026.724.203728/extra")
assert_equal("${parsed_deep}" "" "A URL below a release directory names no release")

polaris_prepared_ffmpeg_classify_base_url(url_fresh "${source_tag}" "" "" "" "")
assert_equal("${url_fresh_ACTION}" "fresh" "No cached base URL is a fresh tree")
assert_equal("${url_fresh_URL}" "${derived_url}" "A fresh tree derives the base URL from the tag")

polaris_prepared_ffmpeg_classify_base_url(url_same "${source_tag}" "${derived_url}" "STRING" "${derived_url}" "")
assert_equal("${url_same_ACTION}" "unchanged" "A cached base URL for the effective tag is unchanged")

polaris_prepared_ffmpeg_classify_base_url(url_stale "${source_tag}" "${release_root}/v2026.724.203728" "STRING" "${release_root}/v2026.724.203728" "")
assert_equal("${url_stale_ACTION}" "recomputed" "A base URL this file derived for the old tag follows the new tag")
assert_equal("${url_stale_URL}" "${derived_url}" "It is recomputed from the effective tag")

polaris_prepared_ffmpeg_classify_base_url(url_legacy "test-release-b" "${release_root}/v2026.724.203728" "STRING" "" "")
assert_equal("${url_legacy_ACTION}" "recomputed" "An untracked derived base URL follows a tag given now")
assert_equal("${url_legacy_URL}" "${release_root}/test-release-b" "It is recomputed for that tag")

polaris_prepared_ffmpeg_classify_base_url(url_custom "${source_tag}" "file://${warm_root}/mirror" "STRING" "" "")
assert_equal("${url_custom_ACTION}" "custom-kept" "A base URL that names no release is kept")
assert_equal("${url_custom_URL}" "file://${warm_root}/mirror" "It is kept as set")

polaris_prepared_ffmpeg_classify_base_url(url_mixed_now "test-release-b" "${release_root}/${source_tag}" "UNINITIALIZED" "" "")
assert_equal("${url_mixed_now_ACTION}" "refuse-mixed" "A base URL given now for another release than the tag is stopped")
assert_contains("${url_mixed_now_MESSAGE}" "names release ${source_tag}" "The stop names the URL's release")
assert_contains("${url_mixed_now_MESSAGE}" "RELEASE_TAG is test-release-b" "The stop names the tag")

polaris_prepared_ffmpeg_classify_base_url(url_mixed_kept "${source_tag}" "${release_root}/test-release-b" "STRING" "${release_root}/v2026.724.203728" "")
assert_equal("${url_mixed_kept_ACTION}" "refuse-mixed" "A persisted base URL somebody set to another release is stopped, not silently rewritten")

polaris_prepared_ffmpeg_classify_base_url(url_chosen_same "test-release-b" "${derived_url}" "STRING" "${derived_url}" "${derived_url}")
assert_equal("${url_chosen_same_ACTION}" "refuse-mixed" "A recorded base URL choice equal to the old derived URL is not rewritten when the tag moves")
assert_equal("${url_chosen_same_URL}" "${derived_url}" "The chosen URL is preserved")

polaris_prepared_ffmpeg_classify_base_url(url_chosen_current "${source_tag}" "${derived_url}" "STRING" "${derived_url}" "${derived_url}")
assert_equal("${url_chosen_current_ACTION}" "unchanged" "A recorded choice equal to the current derived URL needs nothing")

polaris_prepared_ffmpeg_download_refusal(no_refusal "${source_tag}" "${derived_url}")
assert_equal("${no_refusal}" "" "A consistent tag and base URL may download")
polaris_prepared_ffmpeg_download_refusal(mixed_refusal "test-release-b" "${derived_url}")
assert_contains("${mixed_refusal}" "names release ${source_tag}" "The download guard stops a mismatched base URL")
polaris_prepared_ffmpeg_download_refusal(mirror_refusal "test-release-b" "file://${warm_root}/mirror")
assert_equal("${mirror_refusal}" "" "The download guard accepts a base URL that names no release")

message(STATUS "prepared FFmpeg cmake contract: all cases passed")
