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

if(POLARIS_ENABLE_BROWSER_STREAM)
    find_program(GO_EXECUTABLE go REQUIRED)
    set(BROWSER_STREAM_HELPER_BUILD_ARGUMENTS build -trimpath)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND BROWSER_STREAM_HELPER_BUILD_ARGUMENTS
                -buildmode=pie
                "-ldflags=-linkmode=external -extldflags=-Wl,-z,relro,-z,now")
    endif()
    set(BROWSER_STREAM_HELPER_OUTPUT "${CMAKE_BINARY_DIR}/polaris-browser-stream-helper")
    file(GLOB_RECURSE BROWSER_STREAM_HELPER_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/browser_stream_helper/*.go"
            "${CMAKE_SOURCE_DIR}/browser_stream_helper/go.mod"
            "${CMAKE_SOURCE_DIR}/browser_stream_helper/go.sum")
    add_custom_command(
            OUTPUT "${BROWSER_STREAM_HELPER_OUTPUT}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/browser_stream_helper"
            COMMENT "Building Browser Stream WebTransport helper"
            COMMAND "${GO_EXECUTABLE}" ${BROWSER_STREAM_HELPER_BUILD_ARGUMENTS} -o "${BROWSER_STREAM_HELPER_OUTPUT}" .
            DEPENDS ${BROWSER_STREAM_HELPER_SOURCE_FILES}
            VERBATIM)
    add_custom_target(browser-stream-helper ALL DEPENDS "${BROWSER_STREAM_HELPER_OUTPUT}")
    add_dependencies(polaris browser-stream-helper)

    if(UNIX AND NOT APPLE)
        install(PROGRAMS "${BROWSER_STREAM_HELPER_OUTPUT}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()

if(POLARIS_BUILD_MULTISEAT_WORKER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
            NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR
                "POLARIS_BUILD_MULTISEAT_WORKER currently supports only Linux/amd64 locked images")
    endif()
    find_program(POLARIS_GO_EXECUTABLE go REQUIRED)
    set(MULTISEAT_WORKER_OUTPUT "${CMAKE_BINARY_DIR}/polaris-seat-worker")
    set(MULTISEAT_RUNTIME_HELPER_OUTPUT "${CMAKE_BINARY_DIR}/polaris-seat-runtime")
    set(MULTISEAT_SESSION_BUS_PROVIDER_OUTPUT
            "${CMAKE_BINARY_DIR}/polaris-seat-session-bus")
    set(MULTISEAT_AUDIO_PROVIDER_OUTPUT
            "${CMAKE_BINARY_DIR}/polaris-seat-audio")
    set(MULTISEAT_DISPLAY_CAPTURE_PROVIDER_OUTPUT
            "${CMAKE_BINARY_DIR}/polaris-seat-display-capture")
    set(MULTISEAT_NESTED_COMPOSITOR_PROVIDER_OUTPUT
            "${CMAKE_BINARY_DIR}/polaris-seat-nested-compositor")
    file(GLOB_RECURSE MULTISEAT_WORKER_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/multiseat_worker/*.go"
            "${CMAKE_SOURCE_DIR}/multiseat_worker/go.mod"
            "${CMAKE_SOURCE_DIR}/multiseat_worker/internal/seatruntime/testdata/*.json")
    list(APPEND MULTISEAT_WORKER_SOURCES
            "${CMAKE_SOURCE_DIR}/containers/multiseat/Containerfile"
            "${CMAKE_SOURCE_DIR}/containers/multiseat/images.lock.json")
    add_custom_command(
            OUTPUT
                    "${MULTISEAT_WORKER_OUTPUT}"
                    "${MULTISEAT_RUNTIME_HELPER_OUTPUT}"
                    "${MULTISEAT_SESSION_BUS_PROVIDER_OUTPUT}"
                    "${MULTISEAT_AUDIO_PROVIDER_OUTPUT}"
                    "${MULTISEAT_DISPLAY_CAPTURE_PROVIDER_OUTPUT}"
                    "${MULTISEAT_NESTED_COMPOSITOR_PROVIDER_OUTPUT}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/multiseat_worker"
            COMMENT "Building isolated multiseat worker, dispatcher, and private base providers"
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" test -trimpath ./...
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_WORKER_OUTPUT}" .
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_RUNTIME_HELPER_OUTPUT}" ./cmd/polaris-seat-runtime
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_SESSION_BUS_PROVIDER_OUTPUT}"
                    ./cmd/polaris-seat-session-bus
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_AUDIO_PROVIDER_OUTPUT}"
                    ./cmd/polaris-seat-audio
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_DISPLAY_CAPTURE_PROVIDER_OUTPUT}"
                    ./cmd/polaris-seat-display-capture
            COMMAND "${CMAKE_COMMAND}" -E env
                    CGO_ENABLED=0 GOOS=linux GOARCH=amd64
                    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
                    "${POLARIS_GO_EXECUTABLE}" build -trimpath -buildvcs=false
                    "-ldflags=-buildid= -s -w"
                    -o "${MULTISEAT_NESTED_COMPOSITOR_PROVIDER_OUTPUT}"
                    ./cmd/polaris-seat-nested-compositor
            DEPENDS ${MULTISEAT_WORKER_SOURCES}
            VERBATIM)
    add_custom_target(multiseat-worker ALL DEPENDS
            "${MULTISEAT_WORKER_OUTPUT}"
            "${MULTISEAT_RUNTIME_HELPER_OUTPUT}"
            "${MULTISEAT_SESSION_BUS_PROVIDER_OUTPUT}"
            "${MULTISEAT_AUDIO_PROVIDER_OUTPUT}"
            "${MULTISEAT_DISPLAY_CAPTURE_PROVIDER_OUTPUT}"
            "${MULTISEAT_NESTED_COMPOSITOR_PROVIDER_OUTPUT}")
    install(PROGRAMS
            "${MULTISEAT_WORKER_OUTPUT}"
            "${MULTISEAT_RUNTIME_HELPER_OUTPUT}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(PROGRAMS "${MULTISEAT_SESSION_BUS_PROVIDER_OUTPUT}"
            DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/polaris-seat"
            RENAME "session-bus")
    install(PROGRAMS "${MULTISEAT_AUDIO_PROVIDER_OUTPUT}"
            DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/polaris-seat"
            RENAME "audio")
    install(PROGRAMS "${MULTISEAT_DISPLAY_CAPTURE_PROVIDER_OUTPUT}"
            DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/polaris-seat"
            RENAME "display-capture")
    install(PROGRAMS "${MULTISEAT_NESTED_COMPOSITOR_PROVIDER_OUTPUT}"
            DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/polaris-seat"
            RENAME "nested-compositor")
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

# src/process / src/ai_optimizer
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
        (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
    set(GCC_RELEASE_ICE_WORKAROUND_FLAGS "")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-O0 ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-U_FORTIFY_SOURCE ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-D_FORTIFY_SOURCE=0 ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-lto ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-unroll-loops ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-tree-vectorize ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-ipa-cp-clone ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-var-tracking ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-var-tracking-assignments ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-ipa-sra ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-inline-functions ")
    string(APPEND GCC_RELEASE_ICE_WORKAROUND_FLAGS "-fno-inline-small-functions ")
    set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/process.cpp"
            "${CMAKE_SOURCE_DIR}/src/ai_optimizer.cpp"
            DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
            PROPERTIES COMPILE_FLAGS "${GCC_RELEASE_ICE_WORKAROUND_FLAGS}")
endif()

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
