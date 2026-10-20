# The release tag and base URL below are cache entries so a person can override them, which also
# means a build tree keeps whatever they were when it was first configured, while the archive
# hashes further down are read from this file every time. That is how a warm tree broke (#203,
# GitHub #791): a tree first configured on an older pin kept that pin's tag and base URL,
# downloaded the old archive and failed the new hash check, and overriding only the tag still
# fetched from the cached base URL. The resolution below runs before anything is downloaded:
# a cached tag still equal to the default this file recorded on an earlier configure moves with
# the source, a tag a person chose stays, and a cached tag this tree cannot account for stops
# with both tags and `cmake --fresh` in the message. The base URL follows the effective tag
# unless it was set to something else on purpose.

# Where a release's assets live. Only the tests point this elsewhere.
if(NOT DEFINED POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL)
  set(POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL
      "https://github.com/LizardByte/build-deps/releases/download")
endif()

# The tag this file pins, read from its own declaration below with the same anchored pattern the
# media provenance collector uses, so the pin is declared exactly once.
function(polaris_prepared_ffmpeg_source_default_release_tag out_var)
  file(STRINGS "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" declarations
      REGEX "^set\\(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"[^\"]+\"")
  list(LENGTH declarations count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR
        "prepared_ffmpeg.cmake must declare POLARIS_PREPARED_FFMPEG_RELEASE_TAG exactly once; "
        "found ${count} declarations")
  endif()
  string(REGEX REPLACE "^set\\(POLARIS_PREPARED_FFMPEG_RELEASE_TAG \"([^\"]+)\".*$" "\\1"
      tag "${declarations}")
  set("${out_var}" "${tag}" PARENT_SCOPE)
endfunction()

function(polaris_prepared_ffmpeg_default_base_url out_var release_tag)
  string(REGEX REPLACE "/$" "" root "${POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL}")
  set("${out_var}" "${root}/${release_tag}" PARENT_SCOPE)
endfunction()

# The release tag a base URL in the default form names, or "" for any other URL (a mirror, a
# local directory, anything not directly under the release root).
function(polaris_prepared_ffmpeg_base_url_release_tag out_var base_url)
  string(REGEX REPLACE "/$" "" root "${POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL}")
  string(REGEX REPLACE "/$" "" url "${base_url}")
  set(prefix "${root}/")
  string(FIND "${url}" "${prefix}" position)
  if(NOT position EQUAL 0)
    set("${out_var}" "" PARENT_SCOPE)
    return()
  endif()
  string(LENGTH "${prefix}" prefix_length)
  string(SUBSTRING "${url}" ${prefix_length} -1 remainder)
  if(remainder MATCHES "^[^/]+$")
    set("${out_var}" "${remainder}" PARENT_SCOPE)
  else()
    set("${out_var}" "" PARENT_SCOPE)
  endif()
endfunction()

# What CMake writes as the help of every cache entry a -D creates, typed or untyped: ProcessCacheArg
# in Source/cmake.cxx, the same text from 3.20 through 4.4. An entry still carrying it was given on
# a command line, now or at an earlier configure, and no declaration or FORCE has rewritten it since.
# That is CMake's own record of where the entry came from, nothing read from a process or guessed
# from a tag; the contract checks the literal against a live configure, so a change in CMake fails
# a test rather than this file's reading. A -C initial cache file or a script gives an entry its own
# help, which this file does not try to interpret.
set(POLARIS_PREPARED_FFMPEG_COMMAND_LINE_HELP "No help, variable specified on the command line.")
# The help this file's declarations give the two entries. An entry carrying one of these was
# written by a declaration of this file, in this or an earlier Polaris, or by a FORCE below.
set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP
    "LizardByte/build-deps release tag used for prepared FFmpeg archives")
set(POLARIS_PREPARED_FFMPEG_BASE_URL_HELP "Base URL for prepared FFmpeg release archives")

# Where a cache entry came from, read from the help CMake stored with it:
#   command-line  a -D, typed or untyped, now or earlier, not rewritten since
#   declaration   a declaration of this file, in any Polaris, or a FORCE rewrite here
#   other         an initial cache file, a script, or anything else this file cannot account for
#   ""            no entry
function(polaris_prepared_ffmpeg_cache_provenance out_var cached_type cached_help declaration_help)
  if(cached_type STREQUAL "")
    set("${out_var}" "" PARENT_SCOPE)
  elseif("${cached_help}" STREQUAL "${POLARIS_PREPARED_FFMPEG_COMMAND_LINE_HELP}")
    set("${out_var}" "command-line" PARENT_SCOPE)
  elseif("${cached_help}" STREQUAL "${declaration_help}")
    set("${out_var}" "declaration" PARENT_SCOPE)
  else()
    set("${out_var}" "other" PARENT_SCOPE)
  endif()
endfunction()

# What the cached release tag means on this tree. Pure, so every shape has a test. Sets
# <prefix>_TAG (the effective tag), <prefix>_ACTION, <prefix>_MESSAGE and <prefix>_GIVEN (whether the
# effective tag was given on a command line, which the tree then records as a choice).
#   cached_tag         the cache entry's value before this file's declaration ran, or ""
#   cached_type        its TYPE, or "" when there was no entry. A -D given on this command line is
#                      still UNINITIALIZED here unless it was typed (-D...:STRING=).
#   cached_provenance  polaris_prepared_ffmpeg_cache_provenance() of the entry. An entry given on a
#                      command line is a choice whatever its type and whenever it was given, which
#                      is what tells a typed tag, even one equal to the default, from an inherited
#                      default; an entry this file declared is the default of its day; anything
#                      else is read conservatively.
#   tracked_tag        the source default an earlier configure of this file recorded, or ""
#   chosen_tag         the tag an earlier configure recorded as given on the command line, or "".
function(polaris_prepared_ffmpeg_classify_release_tag prefix source_default cached_tag cached_type
         cached_provenance tracked_tag chosen_tag)
  set(tag "${source_default}")
  set(message "")
  set(given FALSE)
  if(cached_type STREQUAL "")
    set(action "fresh")
  elseif(cached_type STREQUAL "UNINITIALIZED"
         OR (cached_provenance STREQUAL "command-line" AND cached_tag STREQUAL source_default))
    set(action "explicit")
    set(tag "${cached_tag}")
    set(given TRUE)
  elseif(cached_provenance STREQUAL "command-line")
    set(action "override-kept")
    set(tag "${cached_tag}")
    set(given TRUE)
    string(CONCAT message
        "Prepared FFmpeg release tag ${cached_tag} is kept as this tree's override, given on its "
        "command line; the source default is ${source_default}.")
  elseif(cached_tag STREQUAL source_default)
    set(action "unchanged")
  elseif(NOT chosen_tag STREQUAL "" AND cached_tag STREQUAL chosen_tag)
    set(action "override-kept")
    set(tag "${cached_tag}")
    set(given TRUE)
    string(CONCAT message
        "Prepared FFmpeg release tag ${cached_tag} is kept as this tree's override, given on its "
        "command line; the source default is ${source_default}.")
  elseif(tracked_tag STREQUAL ""
         OR (cached_tag STREQUAL tracked_tag AND NOT cached_provenance STREQUAL "declaration"))
    # A tree with no record of how its tag was chosen, or a record this entry no longer matches: a
    # default an earlier Polaris declared, an edit in a cache editor, or an initial cache file that
    # rewrote the entry since the record was made. Nobody can tell, so say so.
    set(action "refuse")
    set(tag "${cached_tag}")
    string(CONCAT message
        "This build tree caches prepared FFmpeg release tag ${cached_tag}, but the source pins "
        "${source_default} and the tree cannot account for how ${cached_tag} was chosen. "
        "Nothing was downloaded. To adopt the pinned release, reconfigure with cmake --fresh "
        "(or delete CMakeCache.txt). To keep ${cached_tag} deliberately, pass "
        "-DPOLARIS_PREPARED_FFMPEG_RELEASE_TAG=${cached_tag} on the command line once so it is "
        "recorded as your choice.")
  elseif(cached_tag STREQUAL tracked_tag)
    # Declared by this file and equal to the default it recorded: positively a default.
    set(action "migrated")
    string(CONCAT message
        "Prepared FFmpeg release tag moved from ${cached_tag} to ${source_default}: the cached "
        "value was the source default this tree was last configured with, and the source now "
        "pins ${source_default}.")
  else()
    set(action "override-kept")
    set(tag "${cached_tag}")
    string(CONCAT message
        "Prepared FFmpeg release tag ${cached_tag} is kept as this tree's override; the source "
        "default is ${source_default}.")
  endif()
  set("${prefix}_TAG" "${tag}" PARENT_SCOPE)
  set("${prefix}_ACTION" "${action}" PARENT_SCOPE)
  set("${prefix}_MESSAGE" "${message}" PARENT_SCOPE)
  set("${prefix}_GIVEN" "${given}" PARENT_SCOPE)
endfunction()

# What the cached base URL means for the effective tag. Pure, like the tag decision. Sets
# <prefix>_URL, <prefix>_ACTION and <prefix>_MESSAGE.
#   cached_url         the cache entry's value before this file's declaration ran, or ""
#   cached_type        its TYPE, or ""
#   cached_provenance  polaris_prepared_ffmpeg_cache_provenance() of the entry
#   tracked_url        the derived URL an earlier configure of this file wrote, or ""
#   chosen_url         the URL an earlier configure recorded as given on the command line, or ""
function(polaris_prepared_ffmpeg_classify_base_url prefix effective_tag cached_url cached_type
         cached_provenance tracked_url chosen_url)
  polaris_prepared_ffmpeg_default_base_url(derived "${effective_tag}")
  set(url "${derived}")
  set(message "")
  if(cached_type STREQUAL "")
    set(action "fresh")
  else()
    polaris_prepared_ffmpeg_base_url_release_tag(url_tag "${cached_url}")
    if(url_tag STREQUAL "")
      set(action "custom-kept")
      set(url "${cached_url}")
      string(CONCAT message
          "Prepared FFmpeg archives come from ${cached_url}, which is not a release URL; it is "
          "kept as set for release tag ${effective_tag}.")
    elseif(url_tag STREQUAL effective_tag)
      set(action "unchanged")
      set(url "${cached_url}")
    elseif(cached_type STREQUAL "UNINITIALIZED"
           OR cached_provenance STREQUAL "command-line"
           OR (NOT chosen_url STREQUAL "" AND cached_url STREQUAL chosen_url)
           OR (NOT tracked_url STREQUAL "" AND NOT cached_url STREQUAL tracked_url)
           OR cached_provenance STREQUAL "other")
      # Given on a command line, chosen earlier, contradicting the URL this file derived, or of
      # unknown origin: a base URL for one release with a tag for another is a contradiction to
      # report, not to resolve quietly.
      set(action "refuse-mixed")
      set(url "${cached_url}")
      string(CONCAT message
          "POLARIS_PREPARED_FFMPEG_BASE_URL names release ${url_tag} while "
          "POLARIS_PREPARED_FFMPEG_RELEASE_TAG is ${effective_tag}, so the status line and the "
          "download would name different releases. Nothing was downloaded. Set only the tag and "
          "the base URL follows it, set both to the same release, or reconfigure with "
          "cmake --fresh.")
    else()
      # Declared by this file, in this or an earlier Polaris, from a tag that has since moved, and
      # still carrying this file's help: follow the tag. Every other origin stopped above.
      set(action "recomputed")
      string(CONCAT message
          "Prepared FFmpeg base URL moved from ${cached_url} to ${derived} to follow release tag "
          "${effective_tag}.")
    endif()
  endif()
  set("${prefix}_URL" "${url}" PARENT_SCOPE)
  set("${prefix}_ACTION" "${action}" PARENT_SCOPE)
  set("${prefix}_MESSAGE" "${message}" PARENT_SCOPE)
endfunction()

# Capture what the cache held before the declarations below create or type the entries.
set(_polaris_prepared_ffmpeg_cached_tag "")
set(_polaris_prepared_ffmpeg_cached_tag_type "")
set(_polaris_prepared_ffmpeg_cached_tag_help "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_RELEASE_TAG})
  get_property(_polaris_prepared_ffmpeg_cached_tag CACHE POLARIS_PREPARED_FFMPEG_RELEASE_TAG
      PROPERTY VALUE)
  get_property(_polaris_prepared_ffmpeg_cached_tag_type CACHE POLARIS_PREPARED_FFMPEG_RELEASE_TAG
      PROPERTY TYPE)
  get_property(_polaris_prepared_ffmpeg_cached_tag_help CACHE POLARIS_PREPARED_FFMPEG_RELEASE_TAG
      PROPERTY HELPSTRING)
endif()
polaris_prepared_ffmpeg_cache_provenance(_polaris_prepared_ffmpeg_cached_tag_provenance
    "${_polaris_prepared_ffmpeg_cached_tag_type}"
    "${_polaris_prepared_ffmpeg_cached_tag_help}"
    "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}")
set(_polaris_prepared_ffmpeg_tracked_tag "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG})
  get_property(_polaris_prepared_ffmpeg_tracked_tag
      CACHE POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG PROPERTY VALUE)
endif()
set(_polaris_prepared_ffmpeg_chosen_tag "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG})
  get_property(_polaris_prepared_ffmpeg_chosen_tag
      CACHE POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG PROPERTY VALUE)
endif()
set(_polaris_prepared_ffmpeg_cached_url "")
set(_polaris_prepared_ffmpeg_cached_url_type "")
set(_polaris_prepared_ffmpeg_cached_url_help "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_BASE_URL})
  get_property(_polaris_prepared_ffmpeg_cached_url CACHE POLARIS_PREPARED_FFMPEG_BASE_URL
      PROPERTY VALUE)
  get_property(_polaris_prepared_ffmpeg_cached_url_type CACHE POLARIS_PREPARED_FFMPEG_BASE_URL
      PROPERTY TYPE)
  get_property(_polaris_prepared_ffmpeg_cached_url_help CACHE POLARIS_PREPARED_FFMPEG_BASE_URL
      PROPERTY HELPSTRING)
endif()
polaris_prepared_ffmpeg_cache_provenance(_polaris_prepared_ffmpeg_cached_url_provenance
    "${_polaris_prepared_ffmpeg_cached_url_type}"
    "${_polaris_prepared_ffmpeg_cached_url_help}"
    "${POLARIS_PREPARED_FFMPEG_BASE_URL_HELP}")
set(_polaris_prepared_ffmpeg_tracked_url "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_TRACKED_BASE_URL})
  get_property(_polaris_prepared_ffmpeg_tracked_url CACHE POLARIS_PREPARED_FFMPEG_TRACKED_BASE_URL
      PROPERTY VALUE)
endif()
set(_polaris_prepared_ffmpeg_chosen_url "")
if(DEFINED CACHE{POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL})
  get_property(_polaris_prepared_ffmpeg_chosen_url CACHE POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL
      PROPERTY VALUE)
endif()

set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "v2026.713.132551" CACHE STRING
    "LizardByte/build-deps release tag used for prepared FFmpeg archives")
polaris_prepared_ffmpeg_source_default_release_tag(_polaris_prepared_ffmpeg_source_default)
polaris_prepared_ffmpeg_classify_release_tag(_polaris_prepared_ffmpeg_tag
    "${_polaris_prepared_ffmpeg_source_default}"
    "${_polaris_prepared_ffmpeg_cached_tag}"
    "${_polaris_prepared_ffmpeg_cached_tag_type}"
    "${_polaris_prepared_ffmpeg_cached_tag_provenance}"
    "${_polaris_prepared_ffmpeg_tracked_tag}"
    "${_polaris_prepared_ffmpeg_chosen_tag}")
set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG_STATE "${_polaris_prepared_ffmpeg_tag_ACTION}")
set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG_DIAGNOSTIC "${_polaris_prepared_ffmpeg_tag_MESSAGE}")
set(POLARIS_PREPARED_FFMPEG_REFUSED_RELEASE_TAG "")
if(_polaris_prepared_ffmpeg_tag_GIVEN)
  set(POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG "${_polaris_prepared_ffmpeg_tag_TAG}" CACHE INTERNAL
      "Release tag given on this tree's command line; it stays a choice even when the source default moves")
endif()
if(_polaris_prepared_ffmpeg_tag_ACTION STREQUAL "migrated")
  set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "${_polaris_prepared_ffmpeg_tag_TAG}" CACHE STRING
      "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_HELP}" FORCE)
  unset(POLARIS_PREPARED_FFMPEG_CHOSEN_RELEASE_TAG CACHE)
  message(STATUS "${_polaris_prepared_ffmpeg_tag_MESSAGE}")
elseif(_polaris_prepared_ffmpeg_tag_ACTION STREQUAL "override-kept")
  message(STATUS "${_polaris_prepared_ffmpeg_tag_MESSAGE}")
elseif(_polaris_prepared_ffmpeg_tag_ACTION STREQUAL "refuse")
  # Refused where a download would start, so a local FFMPEG_PREPARED_BINARIES or disabled
  # downloads still work on this tree.
  set(POLARIS_PREPARED_FFMPEG_REFUSED_RELEASE_TAG "${_polaris_prepared_ffmpeg_cached_tag}")
endif()
# The record that lets a cached default move with the source later. Written only where the value
# is positively this file's default: an entry this file created or declared, one it migrated, or a
# choice it recorded. An unchanged value of unknown origin leaves no record and inherits none, so a
# later move stops and asks rather than rewriting what may have been a choice.
if(_polaris_prepared_ffmpeg_tag_ACTION STREQUAL "fresh"
    OR _polaris_prepared_ffmpeg_tag_ACTION STREQUAL "explicit"
    OR _polaris_prepared_ffmpeg_tag_ACTION STREQUAL "migrated"
    OR _polaris_prepared_ffmpeg_tag_ACTION STREQUAL "override-kept"
    OR (_polaris_prepared_ffmpeg_tag_ACTION STREQUAL "unchanged"
        AND _polaris_prepared_ffmpeg_cached_tag_provenance STREQUAL "declaration"))
  set(POLARIS_PREPARED_FFMPEG_TRACKED_DEFAULT_RELEASE_TAG
      "${_polaris_prepared_ffmpeg_source_default}" CACHE INTERNAL
      "Source default release tag when this tree was last configured; a cached tag equal to it moves with the source, any other cached tag is kept as a choice")
endif()

set(POLARIS_PREPARED_FFMPEG_BASE_URL
    "${POLARIS_PREPARED_FFMPEG_RELEASE_ROOT_URL}/${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}"
    CACHE STRING "Base URL for prepared FFmpeg release archives")
polaris_prepared_ffmpeg_classify_base_url(_polaris_prepared_ffmpeg_url
    "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}"
    "${_polaris_prepared_ffmpeg_cached_url}"
    "${_polaris_prepared_ffmpeg_cached_url_type}"
    "${_polaris_prepared_ffmpeg_cached_url_provenance}"
    "${_polaris_prepared_ffmpeg_tracked_url}"
    "${_polaris_prepared_ffmpeg_chosen_url}")
if(_polaris_prepared_ffmpeg_url_ACTION STREQUAL "recomputed")
  set(POLARIS_PREPARED_FFMPEG_BASE_URL "${_polaris_prepared_ffmpeg_url_URL}" CACHE STRING
      "${POLARIS_PREPARED_FFMPEG_BASE_URL_HELP}" FORCE)
  unset(POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL CACHE)
  message(STATUS "${_polaris_prepared_ffmpeg_url_MESSAGE}")
elseif(_polaris_prepared_ffmpeg_url_ACTION STREQUAL "custom-kept")
  message(STATUS "${_polaris_prepared_ffmpeg_url_MESSAGE}")
endif()
if((_polaris_prepared_ffmpeg_cached_url_type STREQUAL "UNINITIALIZED"
    OR _polaris_prepared_ffmpeg_cached_url_provenance STREQUAL "command-line")
    AND (_polaris_prepared_ffmpeg_url_ACTION STREQUAL "unchanged"
         OR _polaris_prepared_ffmpeg_url_ACTION STREQUAL "custom-kept"))
  # Given on a command line and accepted: a choice, even when it equals the derived URL.
  set(POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL "${_polaris_prepared_ffmpeg_url_URL}" CACHE INTERNAL
      "Base URL given on this tree's command line; it stays a choice even when the release tag moves")
elseif(_polaris_prepared_ffmpeg_url_ACTION STREQUAL "fresh")
  unset(POLARIS_PREPARED_FFMPEG_CHOSEN_BASE_URL CACHE)
endif()
# The record that lets a derived URL follow the tag later: only a URL this file derived or declared.
if(_polaris_prepared_ffmpeg_url_ACTION STREQUAL "fresh"
    OR _polaris_prepared_ffmpeg_url_ACTION STREQUAL "recomputed"
    OR (_polaris_prepared_ffmpeg_url_ACTION STREQUAL "unchanged"
        AND _polaris_prepared_ffmpeg_cached_url_provenance STREQUAL "declaration"))
  set(POLARIS_PREPARED_FFMPEG_TRACKED_BASE_URL "${_polaris_prepared_ffmpeg_url_URL}" CACHE INTERNAL
      "Base URL this file derived from the release tag when this tree was last configured; a cached URL equal to it follows the tag, any other cached URL is kept as a choice")
endif()
# A mixed tag and base URL is refused where the download would start, with the message above.

set(POLARIS_PREPARED_FFMPEG_CACHE_DIR
    "${CMAKE_BINARY_DIR}/_deps/prepared-ffmpeg"
    CACHE PATH "Directory for downloaded prepared FFmpeg archives")
option(POLARIS_DOWNLOAD_PREPARED_FFMPEG
    "Download pinned prepared FFmpeg archives when available for this platform" ON)

# The highest NVENC API version Polaris is willing to ship.
#
# libavcodec compiles this version in from the ffnvcodec headers it is built against, then
# refuses at runtime to talk to any driver that reports an older one. Raising this therefore
# raises the minimum NVIDIA driver every Polaris host needs for hardware encoding, and a host
# below it loses NVENC entirely and falls back to VAAPI or software.
#
# 13.0 needs driver 570 or newer. 13.1 needs 610, which Maxwell, Pascal and Volta can never
# reach because 580 is the last branch NVIDIA ships for them.
set(POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION "13.0" CACHE STRING
    "Highest NVENC API version a prepared FFmpeg archive may be built against")

function(polaris_prepared_ffmpeg_asset_name out_var system_name system_processor)
  string(TOLOWER "${system_processor}" processor)
  set(platform "")

  if(system_name STREQUAL "Darwin")
    if(processor STREQUAL "arm64" OR processor STREQUAL "aarch64")
      set(platform "Darwin-arm64")
    elseif(processor STREQUAL "x86_64" OR processor STREQUAL "amd64")
      set(platform "Darwin-x86_64")
    endif()
  elseif(system_name STREQUAL "FreeBSD")
    if(processor STREQUAL "arm64" OR processor STREQUAL "aarch64")
      set(platform "FreeBSD-aarch64")
    elseif(processor STREQUAL "x86_64" OR processor STREQUAL "amd64")
      set(platform "FreeBSD-amd64")
    endif()
  elseif(system_name STREQUAL "Linux")
    if(processor STREQUAL "arm64" OR processor STREQUAL "aarch64")
      set(platform "Linux-aarch64")
    elseif(processor STREQUAL "ppc64le")
      set(platform "Linux-ppc64le")
    elseif(processor STREQUAL "x86_64" OR processor STREQUAL "amd64")
      set(platform "Linux-x86_64")
    endif()
  elseif(system_name STREQUAL "Windows")
    if(processor STREQUAL "arm64" OR processor STREQUAL "aarch64")
      set(platform "Windows-ARM64")
    elseif(processor STREQUAL "x86_64" OR processor STREQUAL "amd64")
      set(platform "Windows-AMD64")
    endif()
  endif()

  if(platform)
    set("${out_var}" "${platform}-ffmpeg.tar.gz" PARENT_SCOPE)
  else()
    set("${out_var}" "" PARENT_SCOPE)
  endif()
endfunction()

function(polaris_prepared_ffmpeg_asset_hash out_var asset_name)
  string(REGEX REPLACE "-ffmpeg\\.tar\\.gz$" "" asset_key "${asset_name}")
  string(MAKE_C_IDENTIFIER "${asset_key}" asset_variable_key)
  set(override_variable "POLARIS_PREPARED_FFMPEG_SHA256_${asset_variable_key}")

  if(DEFINED "${override_variable}")
    set("${out_var}" "${${override_variable}}" PARENT_SCOPE)
    return()
  endif()

  set(hash "")
  if(asset_name STREQUAL "Darwin-arm64-ffmpeg.tar.gz")
    set(hash "056122301edcdec74e00cfa9a3091bf3135d5fe1472234ce7f46426325081bca")
  elseif(asset_name STREQUAL "Darwin-x86_64-ffmpeg.tar.gz")
    set(hash "5b15f4283a2aa94d42abfd55e361cd4520021a7499fcbd693d3533f3ecb0904e")
  elseif(asset_name STREQUAL "FreeBSD-aarch64-ffmpeg.tar.gz")
    set(hash "0adc7baead743be37ae66ff92c634764f5418fae3d5c5ea2ad4ec962cd45c3ce")
  elseif(asset_name STREQUAL "FreeBSD-amd64-ffmpeg.tar.gz")
    set(hash "a4dee66179bd72221f83874beb95afd79ea70159782a53adff0579d494c9f0b3")
  elseif(asset_name STREQUAL "Linux-aarch64-ffmpeg.tar.gz")
    set(hash "2bdcfa663bb7a1b241a47665c94aa288ef2ec40c6a212cc0a8ec63904b886c6d")
  elseif(asset_name STREQUAL "Linux-ppc64le-ffmpeg.tar.gz")
    set(hash "61522f3424311154c6902fc1f427336eff084ff338c7b2d960cfa010183970f7")
  elseif(asset_name STREQUAL "Linux-x86_64-ffmpeg.tar.gz")
    set(hash "66512409857d7c11c18875193c098a5131baec060169c8f8e6397387e7a1af7d")
  elseif(asset_name STREQUAL "Windows-AMD64-ffmpeg.tar.gz")
    set(hash "6bf702af027d849f326823b9cfe058ddc3eff05d5e424624552bcb71c2415c68")
  elseif(asset_name STREQUAL "Windows-ARM64-ffmpeg.tar.gz")
    set(hash "8cc219946f6bf45512612785e518814c22d0e73c8fa1235d7e84a795056c76c1")
  endif()

  set("${out_var}" "${hash}" PARENT_SCOPE)
endfunction()

function(polaris_prepared_ffmpeg_nvenc_api_version out_var prepared_dir)
  set(header "${prepared_dir}/include/ffnvcodec/nvEncodeAPI.h")
  if(NOT EXISTS "${header}")
    # Archives for platforms without NVENC ship no ffnvcodec headers at all.
    set("${out_var}" "" PARENT_SCOPE)
    return()
  endif()

  file(READ "${header}" header_text)
  string(REGEX MATCH "NVENCAPI_MAJOR_VERSION[ \t]+([0-9]+)" _major_match "${header_text}")
  set(major "${CMAKE_MATCH_1}")
  string(REGEX MATCH "NVENCAPI_MINOR_VERSION[ \t]+([0-9]+)" _minor_match "${header_text}")
  set(minor "${CMAKE_MATCH_1}")

  if(major STREQUAL "" OR minor STREQUAL "")
    set("${out_var}" "" PARENT_SCOPE)
    return()
  endif()

  set("${out_var}" "${major}.${minor}" PARENT_SCOPE)
endfunction()

function(polaris_prepared_ffmpeg_nvenc_floor_violation out_var prepared_dir)
  polaris_prepared_ffmpeg_nvenc_api_version(bundled "${prepared_dir}")
  if(bundled STREQUAL "" OR NOT bundled VERSION_GREATER "${POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION}")
    set("${out_var}" "" PARENT_SCOPE)
    return()
  endif()

  string(CONCAT message
      "Prepared FFmpeg ${POLARIS_PREPARED_FFMPEG_RELEASE_TAG} is built against NVENC API "
      "${bundled}, above the ${POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION} Polaris supports. "
      "Every host on an NVIDIA driver older than the one that version requires would lose "
      "hardware encoding silently. Pin an archive built against "
      "${POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION} or older, or raise "
      "POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION deliberately and say which drivers that drops.")
  set("${out_var}" "${message}" PARENT_SCOPE)
endfunction()

function(polaris_validate_prepared_ffmpeg_dir prepared_dir)
  foreach(required_path IN ITEMS
      "include"
      "lib/libavcodec.a"
      "lib/libswscale.a"
      "lib/libavutil.a"
      "lib/libcbs.a")
    if(NOT EXISTS "${prepared_dir}/${required_path}")
      message(FATAL_ERROR
          "FFmpeg prepared binaries are missing ${required_path} at ${prepared_dir}. "
          "Set FFMPEG_PREPARED_BINARIES to a complete prepared FFmpeg directory.")
    endif()
  endforeach()

  polaris_prepared_ffmpeg_nvenc_floor_violation(violation "${prepared_dir}")
  if(NOT violation STREQUAL "")
    message(FATAL_ERROR ${violation})
  endif()
endfunction()

# Why a download must not start with the current tag and base URL, or "" when it may. Checked
# where a download would begin, so a local prepared directory or disabled downloads still work
# on a tree whose cached tag cannot be accounted for.
function(polaris_prepared_ffmpeg_download_refusal out_var release_tag base_url)
  if(POLARIS_PREPARED_FFMPEG_RELEASE_TAG_STATE STREQUAL "refuse"
      AND release_tag STREQUAL "${POLARIS_PREPARED_FFMPEG_REFUSED_RELEASE_TAG}")
    set("${out_var}" "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG_DIAGNOSTIC}" PARENT_SCOPE)
    return()
  endif()
  polaris_prepared_ffmpeg_base_url_release_tag(url_tag "${base_url}")
  if(NOT url_tag STREQUAL "" AND NOT url_tag STREQUAL release_tag)
    polaris_prepared_ffmpeg_classify_base_url(mixed "${release_tag}" "${base_url}" "UNINITIALIZED" "" "" "")
    set("${out_var}" "${mixed_MESSAGE}" PARENT_SCOPE)
    return()
  endif()
  set("${out_var}" "" PARENT_SCOPE)
endfunction()

function(polaris_resolve_prepared_ffmpeg out_var)
  if(DEFINED FFMPEG_PREPARED_BINARIES)
    polaris_validate_prepared_ffmpeg_dir("${FFMPEG_PREPARED_BINARIES}")
    set("${out_var}" "${FFMPEG_PREPARED_BINARIES}" PARENT_SCOPE)
    return()
  endif()

  set(resolved_dir "")

  set(system_processor "${CMAKE_SYSTEM_PROCESSOR}")
  if(NOT system_processor)
    set(system_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()

  if(POLARIS_DOWNLOAD_PREPARED_FFMPEG)
    polaris_prepared_ffmpeg_asset_name(asset_name "${CMAKE_SYSTEM_NAME}" "${system_processor}")
    if(asset_name)
      polaris_prepared_ffmpeg_asset_hash(asset_sha256 "${asset_name}")
    endif()

    if(asset_name AND asset_sha256)
      polaris_prepared_ffmpeg_download_refusal(refusal
          "${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}" "${POLARIS_PREPARED_FFMPEG_BASE_URL}")
      if(NOT refusal STREQUAL "")
        message(FATAL_ERROR "${refusal}")
      endif()

      string(REGEX REPLACE "-ffmpeg\\.tar\\.gz$" "" asset_key "${asset_name}")
      set(download_dir "${POLARIS_PREPARED_FFMPEG_CACHE_DIR}/${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}")
      set(extract_dir "${download_dir}/${asset_key}")
      set(archive_path "${download_dir}/${asset_name}")
      string(REGEX REPLACE "/$" "" base_url "${POLARIS_PREPARED_FFMPEG_BASE_URL}")
      set(download_url "${base_url}/${asset_name}")

      file(MAKE_DIRECTORY "${download_dir}")
      message(STATUS "Using prepared FFmpeg ${asset_name} from ${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}")
      file(DOWNLOAD
          "${download_url}"
          "${archive_path}"
          EXPECTED_HASH "SHA256=${asset_sha256}"
          SHOW_PROGRESS
          STATUS download_status
          LOG download_log)
      list(GET download_status 0 download_code)
      if(NOT download_code EQUAL 0)
        list(GET download_status 1 download_message)
        message(FATAL_ERROR
            "Failed to download prepared FFmpeg archive ${download_url}: ${download_message}\n${download_log}")
      endif()

      if(NOT EXISTS "${extract_dir}/ffmpeg/lib/libavcodec.a")
        file(REMOVE_RECURSE "${extract_dir}")
        file(MAKE_DIRECTORY "${extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${extract_dir}")
      endif()

      set(resolved_dir "${extract_dir}/ffmpeg")
    endif()
  endif()

  if(NOT resolved_dir)
    message(FATAL_ERROR
        "No prepared FFmpeg for ${CMAKE_SYSTEM_NAME}-${system_processor}: prepared "
        "archive downloads are disabled or no pinned archive exists for this platform. "
        "Set FFMPEG_PREPARED_BINARIES to a prepared FFmpeg directory, or enable "
        "POLARIS_DOWNLOAD_PREPARED_FFMPEG on a platform with a pinned archive.")
  endif()

  polaris_validate_prepared_ffmpeg_dir("${resolved_dir}")
  set("${out_var}" "${resolved_dir}" PARENT_SCOPE)
endfunction()
