set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "v2026.713.132551" CACHE STRING
    "LizardByte/build-deps release tag used for prepared FFmpeg archives")
set(POLARIS_PREPARED_FFMPEG_BASE_URL
    "https://github.com/LizardByte/build-deps/releases/download/${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}"
    CACHE STRING "Base URL for prepared FFmpeg release archives")
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
