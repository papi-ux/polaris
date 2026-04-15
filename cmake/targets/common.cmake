# common target definitions
# this file will also load platform specific macros

add_executable(polaris ${POLARIS_TARGET_FILES})
foreach(dep ${POLARIS_TARGET_DEPENDENCIES})
    add_dependencies(polaris ${dep})  # compile these before polaris
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

# todo - is this necessary? ... for anything except linux?
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

target_link_libraries(polaris ${POLARIS_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(polaris PUBLIC ${POLARIS_DEFINITIONS})
set_target_properties(polaris PROPERTIES CXX_STANDARD 23
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS POLARIS_COMPILE_OPTIONS)
        list(APPEND POLARIS_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(polaris PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${POLARIS_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${POLARIS_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301

# Homebrew build fails the vite build if we set these environment variables
if(${POLARIS_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${POLARIS_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

#WebUI build
find_program(NPM npm REQUIRED)

if (NPM_OFFLINE)
    set(NPM_INSTALL_FLAGS "--offline")
else()
    set(NPM_INSTALL_FLAGS "")
endif()

file(GLOB_RECURSE WEB_UI_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/*")

# Keep the npm stamp under node_modules so removing node_modules invalidates incremental builds.
set(WEB_UI_NPM_STAMP "${CMAKE_SOURCE_DIR}/node_modules/.polaris-web-ui-npm.stamp")
set(WEB_UI_BUILD_STAMP "${CMAKE_BINARY_DIR}/CMakeFiles/web-ui-build.stamp")

add_custom_command(
        OUTPUT "${WEB_UI_NPM_STAMP}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Installing NPM dependencies for the Web UI"
        COMMAND "$<$<BOOL:${WIN32}>:cmd;/C>" "${NPM}" ci --no-audit --fund=false ${NPM_INSTALL_FLAGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_UI_NPM_STAMP}"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/package.json"
            "${CMAKE_SOURCE_DIR}/package-lock.json"
        COMMAND_EXPAND_LISTS
        VERBATIM)

add_custom_command(
        OUTPUT "${WEB_UI_BUILD_STAMP}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building the Web UI"
        COMMAND "${CMAKE_COMMAND}" -E env "POLARIS_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}" "POLARIS_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}" "POLARIS_ASSETS_DIR=${NPM_ASSETS_DIR}" "$<$<BOOL:${WIN32}>:cmd;/C>" "${NPM}" run build  # cmake-lint: disable=C0301
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_UI_BUILD_STAMP}"
        DEPENDS
            "${WEB_UI_NPM_STAMP}"
            "${CMAKE_SOURCE_DIR}/vite.config.js"
            "${CMAKE_SOURCE_DIR}/eslint.config.js"
            ${WEB_UI_SOURCE_FILES}
        COMMAND_EXPAND_LISTS
        VERBATIM)

add_custom_target(web-ui ALL DEPENDS "${WEB_UI_BUILD_STAMP}")

add_dependencies(polaris web-ui)

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/nanors
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/rswrapper.c"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS "-ftree-vectorize -funroll-loops")

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
