cmake_minimum_required(VERSION 3.20)

# Runs polaris_validate_prepared_ffmpeg_dir against one prepared directory and lets its
# FATAL_ERROR escape, so the caller can prove the guard actually aborts a configure rather
# than only that the helper returns a message.

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/dependencies/prepared_ffmpeg.cmake")

if(NOT DEFINED prepared_dir)
  message(FATAL_ERROR "prepared_dir is required")
endif()

polaris_validate_prepared_ffmpeg_dir("${prepared_dir}")
message(STATUS "accepted ${prepared_dir}")
