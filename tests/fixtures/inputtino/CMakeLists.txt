cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(polaris_inputtino_regressions LANGUAGES CXX)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Standalone inputtino regressions require Linux UAPI headers")
    endif()
    enable_testing()
    find_package(Threads REQUIRED)
    get_filename_component(polaris_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
    include("${polaris_root}/cmake/dependencies/inputtino.cmake")
    set(POLARIS_INPUTTINO_SOURCE_DIR "${CMAKE_BINARY_DIR}/inputtino-source")
    polaris_prepare_inputtino("${polaris_root}/third-party/inputtino" "${POLARIS_INPUTTINO_SOURCE_DIR}")
endif()

add_executable(test_inputtino_reports report_order.cpp)
target_include_directories(test_inputtino_reports PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${POLARIS_INPUTTINO_SOURCE_DIR}/src/uhid/include")
target_compile_definitions(test_inputtino_reports PRIVATE
    POLARIS_TEST_INPUTTINO_SOURCE="${POLARIS_INPUTTINO_SOURCE_DIR}/src/uhid/joypad_ps5.cpp")

add_executable(test_inputtino_device device_lifetime.cpp)
target_compile_definitions(test_inputtino_device PRIVATE
    POLARIS_TEST_UHID_SOURCE="${POLARIS_INPUTTINO_SOURCE_DIR}/src/uhid/include/uhid/uhid.hpp")

foreach(target test_inputtino_reports test_inputtino_device)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${POLARIS_INPUTTINO_SOURCE_DIR}/include")
    target_link_libraries(${target} PRIVATE Threads::Threads)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
endforeach()
