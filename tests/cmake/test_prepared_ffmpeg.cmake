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
elseif(WARM_PHASE STREQUAL "help-probe")
  get_property(help CACHE WARM_HELP_PROBE PROPERTY HELPSTRING)
  message(STATUS "WARM_HELP=[${help}]")
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

# Case: the typed-default defect. A tag given typed and equal to today's default is still a choice:
# when the source later moves its pin, that tree keeps what it was told. Before the help string was
# read, the typed value read as an inherited default and was silently moved with the source.
set(typed_default_tree "${warm_root}/trees/typed-default")
configure_warm_tree("${typed_default_tree}" current "${module}" typed_default_result typed_default_stdout typed_default_stderr
  "-DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${typed_default_result}" "0" "A typed tag equal to the default resolves the pin: ${typed_default_stderr}")
configure_warm_tree("${typed_default_tree}" current "${next_module}" typed_default_moved_result typed_default_moved_stdout typed_default_moved_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_not_contains("${typed_default_moved_stdout}" "release tag moved from" "A typed choice is not migrated because its value equalled the default")
assert_equal("${typed_default_moved_result}" "0" "A typed choice equal to the old default survives a moved pin: ${typed_default_moved_stderr}")
read_cache_entries("${typed_default_tree}" typed_default_cache)
assert_contains("${typed_default_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG:INTERNAL=${source_tag}" "The tree records that the typed tag was given")
assert_contains("${typed_default_moved_stdout}" "${source_tag} is kept as this tree's override, given on its command line" "The reconfigure says the choice is kept")
assert_contains("${typed_default_moved_stdout}" "WARM_RESOLVED=${typed_default_tree}/_deps/prepared-ffmpeg/${source_tag}/Linux-x86_64/ffmpeg" "The chosen archive is still resolved")
read_cache_entries("${typed_default_tree}" typed_default_moved_cache)
assert_contains("${typed_default_moved_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The typed tag stays cached")
assert_contains("${typed_default_moved_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The base URL stays with the chosen tag")

# Case: a tag given with an explicit cache type (-D...:STRING=) is already typed when this file
# runs, so its type cannot tell it from an inherited value. CMake stores where an entry came from in
# its help string: every -D, typed or untyped, carries CMake's command-line literal until a
# declaration or FORCE rewrites it. A typed tag on a tree with no record is therefore a choice, kept
# and recorded, not stopped. (Before this provenance was read, the same configure was stopped with an
# actionable message; the stop remains for entries whose origin the help string cannot establish.)
set(typed_tree "${warm_root}/trees/typed")
configure_warm_tree("${typed_tree}" current "${module}" typed_result typed_stdout typed_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${typed_result}" "0" "A typed tag is a choice given on the command line: ${typed_stderr}")
assert_contains("${typed_stdout}" "WARM_RESOLVED=${typed_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The chosen release was resolved")
read_cache_entries("${typed_tree}" typed_cache)
assert_contains("${typed_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=test-release-b" "The typed value is preserved")
assert_contains("${typed_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG:INTERNAL=test-release-b" "The tree records that the tag was given")
configure_warm_tree("${typed_tree}" current "${module}" typed_kept_result typed_kept_stdout typed_kept_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_equal("${typed_kept_result}" "0" "The recorded choice survives reconfigure: ${typed_kept_stderr}")
assert_contains("${typed_kept_stdout}" "WARM_RESOLVED=${typed_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64/ffmpeg" "The chosen release is still resolved")
assert_not_contains("${typed_kept_stdout}" "moved from" "A choice moves nowhere")

# Case: a typed override given on a tree that already tracks the default is a choice made now, and
# it survives the next pin move like an untyped one.
set(typed_tracked_tree "${warm_root}/trees/typed-tracked")
configure_warm_tree("${typed_tracked_tree}" current "${module}" typed_tracked_first_result typed_tracked_first_stdout typed_tracked_first_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${typed_tracked_first_result}" "0" "A fresh tree on the current source: ${typed_tracked_first_stderr}")
configure_warm_tree("${typed_tracked_tree}" current "${module}" typed_tracked_result typed_tracked_stdout typed_tracked_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=v2026.724.203728
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${legacy_sha256}")
assert_equal("${typed_tracked_result}" "0" "A typed override on a tracked tree resolves that release: ${typed_tracked_stderr}")
assert_contains("${typed_tracked_stdout}" "WARM_RESOLVED=${typed_tracked_tree}/_deps/prepared-ffmpeg/v2026.724.203728/Linux-x86_64/ffmpeg" "The overridden release was resolved")
read_cache_entries("${typed_tracked_tree}" typed_tracked_cache)
assert_contains("${typed_tracked_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG:INTERNAL=v2026.724.203728" "The typed override is recorded as a choice")
assert_contains("${typed_tracked_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/v2026.724.203728" "The derived base URL followed the typed tag")
configure_warm_tree("${typed_tracked_tree}" current "${next_module}" typed_tracked_moved_result typed_tracked_moved_stdout typed_tracked_moved_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${legacy_sha256}")
assert_equal("${typed_tracked_moved_result}" "0" "The typed override survives a moved pin: ${typed_tracked_moved_stderr}")
assert_not_contains("${typed_tracked_moved_stdout}" "release tag moved from" "A typed override is not migrated")
assert_contains("${typed_tracked_moved_stdout}" "WARM_RESOLVED=${typed_tracked_tree}/_deps/prepared-ffmpeg/v2026.724.203728/Linux-x86_64/ffmpeg" "The overridden release is still resolved")

# Case: a tag an initial cache file set with its own help is of an origin this file does not read.
# Equal to the default it is accepted but leaves no record, so a later pin move stops and asks
# instead of rewriting it; different from the default it is stopped at once.
file(WRITE "${warm_root}/initial-default.cmake" "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"${source_tag}\" CACHE STRING \"set by an initial cache file\")\n")
file(WRITE "${warm_root}/initial-other.cmake" "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"test-release-b\" CACHE STRING \"set by an initial cache file\")\n")
set(initial_tree "${warm_root}/trees/initial-cache")
configure_warm_tree("${initial_tree}" current "${module}" initial_result initial_stdout initial_stderr
  -C "${warm_root}/initial-default.cmake"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${initial_result}" "0" "An initial cache file naming the default resolves the pin: ${initial_stderr}")
read_cache_entries("${initial_tree}" initial_cache)
assert_contains("${initial_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The initial cache value is preserved")
assert_not_contains("${initial_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG" "A value of unknown origin leaves no default record")
assert_not_contains("${initial_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG" "Nor a choice record")
configure_warm_tree("${initial_tree}" current "${next_module}" initial_moved_result initial_moved_stdout initial_moved_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(initial_moved_result EQUAL 0)
  message(FATAL_ERROR "A moved pin over a tag of unknown origin must stop, not rewrite it")
endif()
assert_not_contains("${initial_moved_stderr}" "HASH mismatch" "The stop is not a bare hash mismatch")
assert_contains("${initial_moved_stderr}" "caches prepared FFmpeg release tag ${source_tag}" "The stop names the cached tag")
assert_contains("${initial_moved_stderr}" "the source pins test-release-b" "The stop names the moved source tag")
read_cache_entries("${initial_tree}" initial_moved_cache)
assert_contains("${initial_moved_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The stop preserves the value")
set(initial_other_tree "${warm_root}/trees/initial-cache-other")
configure_warm_tree("${initial_other_tree}" current "${module}" initial_other_result initial_other_stdout initial_other_stderr
  -C "${warm_root}/initial-other.cmake"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(initial_other_result EQUAL 0)
  message(FATAL_ERROR "A non-default tag of unknown origin must stop")
endif()
assert_contains("${initial_other_stderr}" "caches prepared FFmpeg release tag test-release-b" "The stop names the cached tag")
assert_not_exists("${initial_other_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded")

# Case: a record cannot vouch for an entry whose help now says another origin. A tree that tracked
# the default has that same value rewritten by an initial cache file (FORCE, its own help); when the
# pin moves, the record no longer accounts for the entry, so the move stops instead of rewriting
# what may have become a choice.
file(WRITE "${warm_root}/rehelp-tag.cmake" "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"${source_tag}\" CACHE STRING \"rewritten by an initial cache file\" FORCE)\n")
set(rehelp_tree "${warm_root}/trees/rehelp-tag")
configure_warm_tree("${rehelp_tree}" current "${module}" rehelp_first_result rehelp_first_stdout rehelp_first_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${rehelp_first_result}" "0" "A fresh tree on the current source: ${rehelp_first_stderr}")
read_cache_entries("${rehelp_tree}" rehelp_tracked_cache)
assert_contains("${rehelp_tracked_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG:INTERNAL=${source_tag}" "The tree tracks the default")
configure_warm_tree("${rehelp_tree}" current "${module}" rehelp_result rehelp_stdout rehelp_stderr
  -C "${warm_root}/rehelp-tag.cmake"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${rehelp_result}" "0" "The same default rewritten with another help still configures on the same source: ${rehelp_stderr}")
assert_not_contains("${rehelp_stdout}" "moved from" "Nothing moves on the same source")
configure_warm_tree("${rehelp_tree}" current "${next_module}" rehelp_moved_result rehelp_moved_stdout rehelp_moved_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_not_contains("${rehelp_moved_stdout}" "release tag moved from" "A record cannot move an entry whose help says another origin")
if(rehelp_moved_result EQUAL 0)
  message(FATAL_ERROR "A moved pin over an entry of another origin must stop, even on a tree with a record")
endif()
assert_not_contains("${rehelp_moved_stderr}" "HASH mismatch" "The stop is not a bare hash mismatch")
assert_contains("${rehelp_moved_stderr}" "caches prepared FFmpeg release tag ${source_tag}" "The stop names the cached tag")
assert_contains("${rehelp_moved_stderr}" "the source pins test-release-b" "The stop names the moved source tag")
assert_contains("${rehelp_moved_stderr}" "cmake --fresh" "The stop names the way out")
assert_not_exists("${rehelp_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the moved pin")
read_cache_entries("${rehelp_tree}" rehelp_moved_cache)
assert_contains("${rehelp_moved_cache}" "POLARIS_PREPARED_FFMPEG_RELEASE_TAG:STRING=${source_tag}" "The stop preserves the value")

# Case: the same for the base URL. A tracked tree has its derived URL rewritten with another help;
# a tag given later must not recompute it, so the pair stops.
file(WRITE "${warm_root}/rehelp-url.cmake" "set(POLARIS_PREPARED_FFMPEG_BASE_URL \"${release_root}/${source_tag}\" CACHE STRING \"rewritten by an initial cache file\" FORCE)\n")
set(rehelp_url_tree "${warm_root}/trees/rehelp-url")
configure_warm_tree("${rehelp_url_tree}" current "${module}" rehelp_url_first_result rehelp_url_first_stdout rehelp_url_first_stderr
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${rehelp_url_first_result}" "0" "A fresh tree on the current source: ${rehelp_url_first_stderr}")
configure_warm_tree("${rehelp_url_tree}" current "${module}" rehelp_url_result rehelp_url_stdout rehelp_url_stderr
  -C "${warm_root}/rehelp-url.cmake"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${rehelp_url_result}" "0" "The same derived URL rewritten with another help still configures: ${rehelp_url_stderr}")
configure_warm_tree("${rehelp_url_tree}" current "${module}" rehelp_url_moved_result rehelp_url_moved_stdout rehelp_url_moved_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
assert_not_contains("${rehelp_url_moved_stdout}" "base URL moved from" "A record cannot recompute a URL whose help says another origin")
if(rehelp_url_moved_result EQUAL 0)
  message(FATAL_ERROR "A tag given over a base URL of another origin must stop, even on a tree with a record")
endif()
assert_contains("${rehelp_url_moved_stderr}" "names release ${source_tag} while POLARIS_PREPARED_FFMPEG_RELEASE_TAG is test-release-b" "The stop names both releases")
assert_not_exists("${rehelp_url_tree}/_deps/prepared-ffmpeg/test-release-b/Linux-x86_64-ffmpeg.tar.gz" "Nothing was downloaded for the pair")
read_cache_entries("${rehelp_url_tree}" rehelp_url_moved_cache)
assert_contains("${rehelp_url_moved_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The rewritten URL is preserved")

# Case: the help string this file relies on is what CMake actually writes for a -D, typed and
# untyped, on this CMake. A change in CMake fails here, not in a user's build tree.
set(help_tree "${warm_root}/trees/help-probe")
configure_warm_tree("${help_tree}" help-probe "${module}" help_typed_result help_typed_stdout help_typed_stderr
  -DWARM_HELP_PROBE:STRING=typed)
assert_equal("${help_typed_result}" "0" "The help probe configures: ${help_typed_stderr}")
assert_contains("${help_typed_stdout}" "WARM_HELP=[${POLARIS_PREPARED_FFMPEG_COMMAND_LINE_HELP}]" "A typed -D carries CMake's command-line help")
set(help_untyped_tree "${warm_root}/trees/help-probe-untyped")
configure_warm_tree("${help_untyped_tree}" help-probe "${module}" help_untyped_result help_untyped_stdout help_untyped_stderr
  -DWARM_HELP_PROBE=untyped)
assert_equal("${help_untyped_result}" "0" "The help probe configures: ${help_untyped_stderr}")
assert_contains("${help_untyped_stdout}" "WARM_HELP=[${POLARIS_PREPARED_FFMPEG_COMMAND_LINE_HELP}]" "An untyped -D carries the same help")
polaris_prepared_ffmpeg_cache_provenance(command_line_provenance "STRING" "${POLARIS_PREPARED_FFMPEG_COMMAND_LINE_HELP}" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}")
assert_equal("${command_line_provenance}" "command-line" "The literal reads as command-line provenance")
polaris_prepared_ffmpeg_cache_provenance(declaration_provenance "STRING" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}")
assert_equal("${declaration_provenance}" "declaration" "This file's help reads as declaration provenance")
polaris_prepared_ffmpeg_cache_provenance(other_provenance "STRING" "set by an initial cache file" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}")
assert_equal("${other_provenance}" "other" "Any other help reads as unknown provenance")
polaris_prepared_ffmpeg_cache_provenance(no_entry_provenance "" "" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}")
assert_equal("${no_entry_provenance}" "" "No entry has no provenance")

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

# Case: the typed base URL shape of the same defect. A base URL given typed and equal to the derived
# URL is a choice; a tag given later must not rewrite it, so the pair stops. Before the help string
# was read, the typed URL read as a derived one and was silently recomputed.
set(typed_url_tree "${warm_root}/trees/typed-url")
configure_warm_tree("${typed_url_tree}" current "${module}" typed_url_result typed_url_stdout typed_url_stderr
  "-DPOLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}"
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${current_sha256}")
assert_equal("${typed_url_result}" "0" "A typed base URL equal to the derived one resolves the pin: ${typed_url_stderr}")
read_cache_entries("${typed_url_tree}" typed_url_cache)
assert_contains("${typed_url_cache}" "POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL:INTERNAL=${release_root}/${source_tag}" "The tree records that the typed base URL was given")
assert_not_contains("${typed_url_cache}" "POLARIS_PREPARED_FFMPEG_TRACKED_BASE_URL" "A typed base URL is not recorded as derived")
configure_warm_tree("${typed_url_tree}" current "${module}" typed_url_moved_result typed_url_moved_stdout typed_url_moved_stderr
  -DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=test-release-b
  "-DPOLARIS_PREPARED_FFMPEG_SHA256_Linux_x86_64=${next_sha256}")
if(typed_url_moved_result EQUAL 0)
  message(FATAL_ERROR "A typed base URL must not be rewritten to follow a tag given later")
endif()
assert_not_contains("${typed_url_moved_stdout}" "base URL moved from" "The typed base URL is not recomputed")
assert_contains("${typed_url_moved_stderr}" "names release ${source_tag} while POLARIS_PREPARED_FFMPEG_RELEASE_TAG is test-release-b" "The stop names both releases")
read_cache_entries("${typed_url_tree}" typed_url_moved_cache)
assert_contains("${typed_url_moved_cache}" "POLARIS_PREPARED_FFMPEG_BASE_URL:STRING=${release_root}/${source_tag}" "The typed base URL is preserved")

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
file(READ "${module}" module_text_for_help)
string(FIND "${module_text_for_help}" "set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"${source_tag}\" CACHE STRING\n    \"${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}\")" tag_help_position)
if(tag_help_position EQUAL -1)
  message(FATAL_ERROR "The release tag declaration must carry the help the provenance reads as this file's own")
endif()
file(STRINGS "${module}" url_help_lines REGEX "CACHE STRING \"${POLARIS_PREPARED_FFMPEG_BASE_URL_HELP}\"\\)$")
list(LENGTH url_help_lines url_help_count)
assert_equal("${url_help_count}" "1" "The base URL declaration carries the help the provenance reads as this file's own")
assert_equal("${parsed_default}" "${source_tag}" "The module reads its own declaration")
file(STRINGS "${module}" linux_hash_lines REGEX "66512409857d7c11c18875193c098a5131baec060169c8f8e6397387e7a1af7d")
list(LENGTH linux_hash_lines linux_hash_count)
assert_equal("${linux_hash_count}" "1" "The Linux x86_64 hash of the pin is declared exactly once")

# The two decisions are pure functions. Each shape they can see has a case here, including the
# ones a build tree cannot conveniently be put into.

polaris_prepared_ffmpeg_classify_release_tag(fresh_case "${source_tag}" "" "" "" "" "")
assert_equal("${fresh_case_ACTION}" "fresh" "No cache entry is a fresh tree")
assert_equal("${fresh_case_TAG}" "${source_tag}" "A fresh tree takes the source default")

polaris_prepared_ffmpeg_classify_release_tag(same_case "${source_tag}" "${source_tag}" "STRING" "declaration" "${source_tag}" "")
assert_equal("${same_case_ACTION}" "unchanged" "A cached tag equal to the source default is unchanged")

polaris_prepared_ffmpeg_classify_release_tag(same_untracked_case "${source_tag}" "${source_tag}" "STRING" "declaration" "" "")
assert_equal("${same_untracked_case_ACTION}" "unchanged" "A cached tag equal to the source default needs no record")

polaris_prepared_ffmpeg_classify_release_tag(tracked_case "test-release-b" "${source_tag}" "STRING" "declaration" "${source_tag}" "")
assert_equal("${tracked_case_ACTION}" "migrated" "A cached tag equal to the recorded default moves with the source")
assert_equal("${tracked_case_TAG}" "test-release-b" "It moves to the new source default")
assert_contains("${tracked_case_MESSAGE}" "moved from ${source_tag} to test-release-b" "The move names both tags")

polaris_prepared_ffmpeg_classify_release_tag(choice_case "${source_tag}" "my-pin" "STRING" "declaration" "${source_tag}" "")
assert_equal("${choice_case_ACTION}" "override-kept" "A cached tag that differs from the recorded default is a choice")
assert_equal("${choice_case_TAG}" "my-pin" "The choice is kept")

polaris_prepared_ffmpeg_classify_release_tag(old_choice_case "${source_tag}" "v2026.724.203728" "STRING" "declaration" "v2026.516.30821" "")
assert_equal("${old_choice_case_ACTION}" "override-kept" "An earlier pin chosen on a tracked tree is a choice, not a default to move")

polaris_prepared_ffmpeg_classify_release_tag(legacy_case "${source_tag}" "v2026.724.203728" "STRING" "declaration" "" "")
assert_equal("${legacy_case_ACTION}" "refuse" "An untracked tree caching an earlier pin is stopped, not guessed about")
assert_equal("${legacy_case_TAG}" "v2026.724.203728" "The cached tag is reported as the effective one")
assert_contains("${legacy_case_MESSAGE}" "v2026.724.203728" "The stop names the cached tag")
assert_contains("${legacy_case_MESSAGE}" "${source_tag}" "The stop names the source tag")
assert_contains("${legacy_case_MESSAGE}" "cmake --fresh" "The stop names the way out")

# The typed-input ambiguity, decided by provenance rather than type: a STRING entry with CMake's
# command-line help was given on a command line and is a choice; one carrying this file's declaration
# help with no record is the inherited case and is stopped; any other help is stopped as unknown.
polaris_prepared_ffmpeg_classify_release_tag(typed_case "${source_tag}" "my-pin" "STRING" "command-line" "" "")
assert_equal("${typed_case_ACTION}" "override-kept" "A typed tag with command-line provenance is a choice, kept")
assert_equal("${typed_case_TAG}" "my-pin" "The typed tag is effective")
assert_equal("${typed_case_GIVEN}" "TRUE" "It is recorded as given")
assert_contains("${typed_case_MESSAGE}" "given on its command line" "The message says why it is kept")

polaris_prepared_ffmpeg_classify_release_tag(typed_default_case "${source_tag}" "${source_tag}" "STRING" "command-line" "" "")
assert_equal("${typed_default_case_ACTION}" "explicit" "A typed tag equal to the default with command-line provenance is still a choice")
assert_equal("${typed_default_case_GIVEN}" "TRUE" "It is recorded as given, so a later move keeps it")
polaris_prepared_ffmpeg_classify_release_tag(not_given_case "${source_tag}" "${source_tag}" "STRING" "declaration" "" "")
assert_equal("${not_given_case_GIVEN}" "FALSE" "A declared default is not a choice")

polaris_prepared_ffmpeg_classify_release_tag(inherited_case "${source_tag}" "v2026.724.203728" "STRING" "declaration" "" "")
assert_equal("${inherited_case_ACTION}" "refuse" "A declared value of an earlier Polaris with no record is stopped, not guessed about")

polaris_prepared_ffmpeg_classify_release_tag(tracked_other_case "test-release-b" "${source_tag}" "STRING" "other" "${source_tag}" "")
assert_equal("${tracked_other_case_ACTION}" "refuse" "A recorded default whose entry now carries another help is stopped, not moved")
assert_equal("${tracked_other_case_TAG}" "${source_tag}" "The value is preserved")

polaris_prepared_ffmpeg_classify_release_tag(tracked_declared_case "test-release-b" "${source_tag}" "STRING" "declaration" "${source_tag}" "")
assert_equal("${tracked_declared_case_ACTION}" "migrated" "A recorded default still carrying this file's help moves with the source")

polaris_prepared_ffmpeg_classify_release_tag(unknown_case "${source_tag}" "my-pin" "STRING" "other" "" "")
assert_equal("${unknown_case_ACTION}" "refuse" "A value of unknown origin that differs from the default is stopped")

polaris_prepared_ffmpeg_classify_release_tag(unknown_default_case "${source_tag}" "${source_tag}" "STRING" "other" "" "")
assert_equal("${unknown_default_case_ACTION}" "unchanged" "A value of unknown origin equal to the default is accepted without a record")

polaris_prepared_ffmpeg_classify_release_tag(given_case "${source_tag}" "my-pin" "UNINITIALIZED" "command-line" "" "")
assert_equal("${given_case_ACTION}" "explicit" "A -D on this command line is a choice made now")
assert_equal("${given_case_TAG}" "my-pin" "The given tag is effective")

polaris_prepared_ffmpeg_classify_release_tag(given_tracked_case "${source_tag}" "my-pin" "UNINITIALIZED" "command-line" "${source_tag}" "")
assert_equal("${given_tracked_case_ACTION}" "explicit" "A -D on a tracked tree is a choice made now")

# The URL helpers read the release root the warm trees were given above.
set(POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL "${release_root}")
polaris_prepared_ffmpeg_classify_release_tag(chosen_same_case "test-release-b" "${source_tag}" "STRING" "declaration" "${source_tag}" "${source_tag}")
assert_equal("${chosen_same_case_ACTION}" "override-kept" "A recorded choice equal to the old default stays a choice when the default moves")
assert_equal("${chosen_same_case_TAG}" "${source_tag}" "The chosen tag stays effective")
assert_contains("${chosen_same_case_MESSAGE}" "given on its command line" "The message says why it is kept")

polaris_prepared_ffmpeg_classify_release_tag(chosen_current_case "${source_tag}" "${source_tag}" "STRING" "declaration" "${source_tag}" "${source_tag}")
assert_equal("${chosen_current_case_ACTION}" "unchanged" "A recorded choice equal to the current default needs no message")

polaris_prepared_ffmpeg_classify_release_tag(chosen_stale_case "test-release-b" "${source_tag}" "STRING" "declaration" "${source_tag}" "my-pin")
assert_equal("${chosen_stale_case_ACTION}" "migrated" "A stale choice record for another value does not stop a recorded default from moving")

polaris_prepared_ffmpeg_default_base_url(derived_url "${source_tag}")
assert_equal("${derived_url}" "${release_root}/${source_tag}" "The default base URL is the release root plus the tag")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_tag "${release_root}/v2026.724.203728/")
assert_equal("${parsed_tag}" "v2026.724.203728" "A default-form base URL names its release, with or without a trailing slash")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_mirror "file://${warm_root}/mirror")
assert_equal("${parsed_mirror}" "" "A URL outside the release root names no release")
polaris_prepared_ffmpeg_base_url_release_tag(parsed_deep "${release_root}/v2026.724.203728/extra")
assert_equal("${parsed_deep}" "" "A URL below a release directory names no release")

polaris_prepared_ffmpeg_classify_base_url(url_fresh "${source_tag}" "" "" "" "" "")
assert_equal("${url_fresh_ACTION}" "fresh" "No cached base URL is a fresh tree")
assert_equal("${url_fresh_URL}" "${derived_url}" "A fresh tree derives the base URL from the tag")

polaris_prepared_ffmpeg_classify_base_url(url_same "${source_tag}" "${derived_url}" "STRING" "declaration" "${derived_url}" "")
assert_equal("${url_same_ACTION}" "unchanged" "A cached base URL for the effective tag is unchanged")

polaris_prepared_ffmpeg_classify_base_url(url_stale "${source_tag}" "${release_root}/v2026.724.203728" "STRING" "declaration" "${release_root}/v2026.724.203728" "")
assert_equal("${url_stale_ACTION}" "recomputed" "A base URL this file derived for the old tag follows the new tag")
assert_equal("${url_stale_URL}" "${derived_url}" "It is recomputed from the effective tag")

polaris_prepared_ffmpeg_classify_base_url(url_legacy "test-release-b" "${release_root}/v2026.724.203728" "STRING" "declaration" "" "")
assert_equal("${url_legacy_ACTION}" "recomputed" "An untracked derived base URL follows a tag given now")
assert_equal("${url_legacy_URL}" "${release_root}/test-release-b" "It is recomputed for that tag")

polaris_prepared_ffmpeg_classify_base_url(url_custom "${source_tag}" "file://${warm_root}/mirror" "STRING" "declaration" "" "")
assert_equal("${url_custom_ACTION}" "custom-kept" "A base URL that names no release is kept")
assert_equal("${url_custom_URL}" "file://${warm_root}/mirror" "It is kept as set")

polaris_prepared_ffmpeg_classify_base_url(url_mixed_now "test-release-b" "${release_root}/${source_tag}" "UNINITIALIZED" "command-line" "" "")
assert_equal("${url_mixed_now_ACTION}" "refuse-mixed" "A base URL given now for another release than the tag is stopped")
assert_contains("${url_mixed_now_MESSAGE}" "names release ${source_tag}" "The stop names the URL's release")
assert_contains("${url_mixed_now_MESSAGE}" "RELEASE_TAG is test-release-b" "The stop names the tag")

polaris_prepared_ffmpeg_classify_base_url(url_typed_same "test-release-b" "${derived_url}" "STRING" "command-line" "" "")
assert_equal("${url_typed_same_ACTION}" "refuse-mixed" "A typed base URL for another release than the tag is kept and the pair stopped, not rewritten")
assert_equal("${url_typed_same_URL}" "${derived_url}" "The typed URL is preserved")

polaris_prepared_ffmpeg_classify_base_url(url_tracked_other "test-release-b" "${derived_url}" "STRING" "other" "${derived_url}" "")
assert_equal("${url_tracked_other_ACTION}" "refuse-mixed" "A recorded derived URL whose entry now carries another help is stopped, not recomputed")

polaris_prepared_ffmpeg_classify_base_url(url_unknown "test-release-b" "${derived_url}" "STRING" "other" "" "")
assert_equal("${url_unknown_ACTION}" "refuse-mixed" "A base URL of unknown origin for another release is stopped, not rewritten")

polaris_prepared_ffmpeg_classify_base_url(url_mixed_kept "${source_tag}" "${release_root}/test-release-b" "STRING" "declaration" "${release_root}/v2026.724.203728" "")
assert_equal("${url_mixed_kept_ACTION}" "refuse-mixed" "A persisted base URL somebody set to another release is stopped, not silently rewritten")

polaris_prepared_ffmpeg_classify_base_url(url_chosen_same "test-release-b" "${derived_url}" "STRING" "declaration" "${derived_url}" "${derived_url}")
assert_equal("${url_chosen_same_ACTION}" "refuse-mixed" "A recorded base URL choice equal to the old derived URL is not rewritten when the tag moves")
assert_equal("${url_chosen_same_URL}" "${derived_url}" "The chosen URL is preserved")

polaris_prepared_ffmpeg_classify_base_url(url_chosen_current "${source_tag}" "${derived_url}" "STRING" "declaration" "${derived_url}" "${derived_url}")
assert_equal("${url_chosen_current_ACTION}" "unchanged" "A recorded choice equal to the current derived URL needs nothing")

polaris_prepared_ffmpeg_download_refusal(no_refusal "${source_tag}" "${derived_url}")
assert_equal("${no_refusal}" "" "A consistent tag and base URL may download")
polaris_prepared_ffmpeg_download_refusal(mixed_refusal "test-release-b" "${derived_url}")
assert_contains("${mixed_refusal}" "names release ${source_tag}" "The download guard stops a mismatched base URL")
polaris_prepared_ffmpeg_download_refusal(mirror_refusal "test-release-b" "file://${warm_root}/mirror")
assert_equal("${mirror_refusal}" "" "The download guard accepts a base URL that names no release")

message(STATUS "prepared FFmpeg cmake contract: all cases passed")
