cmake_minimum_required(VERSION 3.20)
project(portal_vaapi_ram_upload_fixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(POLARIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
find_path(MOONLIGHT_INCLUDE_DIR moonlight-common-c/src/Limelight.h HINTS "${POLARIS_ROOT}/third-party" REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FRAME REQUIRED IMPORTED_TARGET libavcodec libswscale libavutil libpipewire-0.3 egl gbm libdrm)
add_executable(portal_vaapi_ram_upload_fixture
  packed_rgb10_upload.cpp
  fixture_dependencies.cpp
  "${POLARIS_ROOT}/src/platform/linux/graphics.cpp"
  "${POLARIS_ROOT}/src/platform/linux/pipewire_capture.cpp"
  "${POLARIS_ROOT}/third-party/glad/src/gl.c"
  "${POLARIS_ROOT}/third-party/glad/src/egl.c")
target_include_directories(portal_vaapi_ram_upload_fixture PRIVATE
  "${POLARIS_ROOT}" "${POLARIS_ROOT}/third-party/glad/include"
  "${NLOHMANN_INCLUDE_DIR}" "${MOONLIGHT_INCLUDE_DIR}")
target_compile_definitions(portal_vaapi_ram_upload_fixture PRIVATE
  BOOST_LOG_DYN_LINK POLARIS_ASSETS_DIR="share/polaris")
target_compile_options(portal_vaapi_ram_upload_fixture PRIVATE -ffunction-sections -fdata-sections)
target_link_options(portal_vaapi_ram_upload_fixture PRIVATE -Wl,--gc-sections)
target_link_libraries(portal_vaapi_ram_upload_fixture PRIVATE PkgConfig::FRAME boost_log boost_log_setup boost_thread ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME portal_vaapi_ram_upload_fixture COMMAND portal_vaapi_ram_upload_fixture)

find_file(MESA_EGL_VENDOR 50_mesa.json PATHS /usr/share/glvnd/egl_vendor.d /etc/glvnd/egl_vendor.d REQUIRED)
add_test(NAME portal_vaapi_ram_software_egl_readback COMMAND ${CMAKE_COMMAND} -E env
  LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless
  __EGL_VENDOR_LIBRARY_FILENAMES=${MESA_EGL_VENDOR}
  $<TARGET_FILE:portal_vaapi_ram_upload_fixture> --software-egl)

# The fixture includes vaapi.cpp to invoke its internal va_ram_t directly.
# Its GL/VAAPI runtime is not initialized; real GL call-site mocks fail conversion.
pkg_check_modules(VA REQUIRED IMPORTED_TARGET libva libva-drm)
add_executable(portal_vaapi_ram_convert_fixture
  vaapi_ram_convert.cpp fixture_dependencies.cpp
  "${POLARIS_ROOT}/src/platform/linux/graphics.cpp"
  "${POLARIS_ROOT}/src/platform/linux/pipewire_capture.cpp"
  "${POLARIS_ROOT}/src/video_colorspace.cpp"
  "${POLARIS_ROOT}/src/file_handler.cpp"
  "${POLARIS_ROOT}/third-party/glad/src/gl.c"
  "${POLARIS_ROOT}/third-party/glad/src/egl.c")
target_include_directories(portal_vaapi_ram_convert_fixture PRIVATE
  "${POLARIS_ROOT}" "${POLARIS_ROOT}/third-party/glad/include"
  "${NLOHMANN_INCLUDE_DIR}" "${MOONLIGHT_INCLUDE_DIR}")
target_compile_definitions(portal_vaapi_ram_convert_fixture PRIVATE
  BOOST_LOG_DYN_LINK POLARIS_ASSETS_DIR="share/polaris")
target_compile_options(portal_vaapi_ram_convert_fixture PRIVATE -ffunction-sections -fdata-sections)
target_link_options(portal_vaapi_ram_convert_fixture PRIVATE -Wl,--gc-sections)
target_link_libraries(portal_vaapi_ram_convert_fixture PRIVATE PkgConfig::FRAME PkgConfig::VA
  boost_log boost_log_setup boost_thread ${CMAKE_DL_LIBS})
add_test(NAME portal_vaapi_ram_convert_fixture COMMAND portal_vaapi_ram_convert_fixture)
