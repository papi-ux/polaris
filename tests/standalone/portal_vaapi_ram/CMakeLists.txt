cmake_minimum_required(VERSION 3.20)
project(portal_vaapi_ram_upload_fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(POLARIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
find_path(MOONLIGHT_INCLUDE_DIR moonlight-common-c/src/Limelight.h REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FRAME REQUIRED IMPORTED_TARGET libavcodec libswscale libavutil libpipewire-0.3 egl gbm libdrm)
add_executable(portal_vaapi_ram_upload_fixture
  packed_rgb10_upload.cpp
  "${POLARIS_ROOT}/src/platform/linux/graphics.cpp"
  "${POLARIS_ROOT}/src/platform/linux/pipewire_capture.cpp")
target_include_directories(portal_vaapi_ram_upload_fixture PRIVATE
  "${POLARIS_ROOT}" "${POLARIS_ROOT}/third-party/glad/include"
  "${NLOHMANN_INCLUDE_DIR}" "${MOONLIGHT_INCLUDE_DIR}")
target_compile_definitions(portal_vaapi_ram_upload_fixture PRIVATE
  BOOST_LOG_DYN_LINK POLARIS_ASSETS_DIR="share/polaris")
target_compile_options(portal_vaapi_ram_upload_fixture PRIVATE -ffunction-sections -fdata-sections)
target_link_options(portal_vaapi_ram_upload_fixture PRIVATE -Wl,--gc-sections)
target_link_libraries(portal_vaapi_ram_upload_fixture PRIVATE PkgConfig::FRAME boost_log boost_thread)
enable_testing()
add_test(NAME portal_vaapi_ram_upload_fixture COMMAND portal_vaapi_ram_upload_fixture)
