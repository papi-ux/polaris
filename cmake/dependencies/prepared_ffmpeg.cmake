set(POLARIS_PREPARED_FFMPEG_RELEASE_TAG "v2026.724.203728" CACHE STRING
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
    set(hash "f4f72fcef4180f18329351cc1080e3fa1a5a7d084fa1c52defa93586aac88f0f")
  elseif(asset_name STREQUAL "Darwin-x86_64-ffmpeg.tar.gz")
    set(hash "da45523c20c0dd44ef3f54ffc41a16f363da2e2298bd0539e3b502b3e750eef7")
  elseif(asset_name STREQUAL "FreeBSD-aarch64-ffmpeg.tar.gz")
    set(hash "3a3527675b09b8537b6997622001df21be61d8d280671dd11a388662699d7ad8")
  elseif(asset_name STREQUAL "FreeBSD-amd64-ffmpeg.tar.gz")
    set(hash "3ca1b26feaa0402b7e89124b0c55ba4013cee31cec8d6ec0ac5e5b846afa0cb0")
  elseif(asset_name STREQUAL "Linux-aarch64-ffmpeg.tar.gz")
    set(hash "fd6492f55d79ae178db97e48d6395b4cac2a2e10b2f157b0d40355cfd7c160e8")
  elseif(asset_name STREQUAL "Linux-ppc64le-ffmpeg.tar.gz")
    set(hash "30583f89fc82816872ed4b9ac044e04c2c2f2a79aa63ca3a9facb5a20e15fcec")
  elseif(asset_name STREQUAL "Linux-x86_64-ffmpeg.tar.gz")
    set(hash "2c27d4694b4ed0e734f497d4bd62f1b3662cbbc4ded2a69f2dc4b703441eebb3")
  elseif(asset_name STREQUAL "Windows-AMD64-ffmpeg.tar.gz")
    set(hash "b293d7f6bd3f032ea01c7e4451b7db540622f2d603e8b98d336513895842c506")
  elseif(asset_name STREQUAL "Windows-ARM64-ffmpeg.tar.gz")
    set(hash "53e0dee93a2185fc619425da14cf10a7a328d016ff9cc50c2c52e051bb3895d1")
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
