set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "v2026.516.30821" CACHE STRING
    "LizardByte/build-deps release tag used for prepared FFmpeg archives")
set(POLARIS_PREPARED_FFMPEG_BASE_URL
    "https://github.com/LizardByte/build-deps/releases/download/${POLARIS_PREPARED_FFMPEG_RELEASE_TAG}"
    CACHE STRING "Base URL for prepared FFmpeg release archives")
set(POLARIS_PREPARED_FFMPEG_CACHE_DIR
    "${CMAKE_BINARY_DIR}/_deps/prepared-ffmpeg"
    CACHE PATH "Directory for downloaded prepared FFmpeg archives")
option(POLARIS_DOWNLOAD_PREPARED_FFMPEG
    "Download pinned prepared FFmpeg archives when available for this platform" ON)

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
    set(hash "707813b81590d80f0f411e05ddbac9a4e2f1c4dbc1e3f61d32d512454317bfa6")
  elseif(asset_name STREQUAL "Darwin-x86_64-ffmpeg.tar.gz")
    set(hash "fce51e3ecb41d60c39b0378a548e786c5e11e6c5834fdb805a6cc531b4458bf0")
  elseif(asset_name STREQUAL "FreeBSD-aarch64-ffmpeg.tar.gz")
    set(hash "7ef5687dadc700f3023b36e46503ebde28dd9d41460e57beb21befffeae423e4")
  elseif(asset_name STREQUAL "FreeBSD-amd64-ffmpeg.tar.gz")
    set(hash "2f1f1510ec121ac8e5aa5ee998bdefecfd09430536c1d5669e29fa2e878621a5")
  elseif(asset_name STREQUAL "Linux-aarch64-ffmpeg.tar.gz")
    set(hash "10b6c9440ba6178ec3b944f6c6f697253497cad5196d23a2d32d335daae0062e")
  elseif(asset_name STREQUAL "Linux-ppc64le-ffmpeg.tar.gz")
    set(hash "8b7b0ba5eaa12bc9bfb6e3561fb14a50453ce688f6c8cab81aa70fb85bc4e3fb")
  elseif(asset_name STREQUAL "Linux-x86_64-ffmpeg.tar.gz")
    set(hash "c32319fcc2867befe8ff3ae26b4d3a58378c3a9c1bfef9029c9969321c3bc6ec")
  elseif(asset_name STREQUAL "Windows-AMD64-ffmpeg.tar.gz")
    set(hash "2f7a2c2fc6be9b96de3c6f654389f73a5e5d369d7e802d017894fae96247661d")
  elseif(asset_name STREQUAL "Windows-ARM64-ffmpeg.tar.gz")
    set(hash "0675898a1c7175e133b8f0e9cdb737038c53b9bd86d1a5b63fe2ea88a974d2e6")
  endif()

  set("${out_var}" "${hash}" PARENT_SCOPE)
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
endfunction()

function(polaris_resolve_prepared_ffmpeg out_var)
  if(DEFINED FFMPEG_PREPARED_BINARIES)
    polaris_validate_prepared_ffmpeg_dir("${FFMPEG_PREPARED_BINARIES}")
    set("${out_var}" "${FFMPEG_PREPARED_BINARIES}" PARENT_SCOPE)
    return()
  endif()

  set(resolved_dir "")

  if(POLARIS_DOWNLOAD_PREPARED_FFMPEG)
    set(system_processor "${CMAKE_SYSTEM_PROCESSOR}")
    if(NOT system_processor)
      set(system_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

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
    set(resolved_dir
        "${CMAKE_SOURCE_DIR}/third-party/build-deps/dist/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  polaris_validate_prepared_ffmpeg_dir("${resolved_dir}")
  set("${out_var}" "${resolved_dir}" PARENT_SCOPE)
endfunction()
