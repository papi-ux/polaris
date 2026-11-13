# Set build variables if env variables are defined
# These are used in configured files such as manifests for different packages
if(DEFINED ENV{BRANCH})
    set(GITHUB_BRANCH $ENV{BRANCH})
endif()
if(DEFINED ENV{BUILD_VERSION})  # cmake-lint: disable=W0106
    set(BUILD_VERSION $ENV{BUILD_VERSION})
endif()
if(DEFINED ENV{CLONE_URL})
    set(GITHUB_CLONE_URL $ENV{CLONE_URL})
endif()
if(DEFINED ENV{COMMIT})
    set(GITHUB_COMMIT $ENV{COMMIT})
endif()
if(DEFINED ENV{TAG})
    set(GITHUB_TAG $ENV{TAG})
endif()

# Check if env vars are defined before attempting to access them, variables will be defined even if blank
if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}))  # cmake-lint: disable=W0106
    if((DEFINED ENV{BRANCH}) AND (NOT $ENV{BUILD_VERSION} STREQUAL ""))
        # If BRANCH is defined and BUILD_VERSION is not empty, then we are building from CI
        # If BRANCH is master we are building a push/release build
        MESSAGE("Got from CI '$ENV{BRANCH}' branch and version '$ENV{BUILD_VERSION}'")
        set(PROJECT_VERSION $ENV{BUILD_VERSION})
        string(REGEX REPLACE "^v" "" PROJECT_VERSION ${PROJECT_VERSION})  # remove the v prefix if it exists
        set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})  # cpack will use this to set the binary versions
    endif()
else()
    # Generate Polaris Version based of the git tag
    # https://github.com/nocnokneo/cmake-git-versioning-example/blob/master/LICENSE
    find_package(Git)
    if(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        set(GIT_SAFE_DIRECTORY_ARGS -c "safe.directory=${CMAKE_SOURCE_DIR}")
        #Get current Branch
        execute_process(
                COMMAND ${GIT_EXECUTABLE} ${GIT_SAFE_DIRECTORY_ARGS} rev-parse --abbrev-ref HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE GIT_DESCRIBE_BRANCH
                RESULT_VARIABLE GIT_DESCRIBE_ERROR_CODE
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        # Gather current commit
        execute_process(
                COMMAND ${GIT_EXECUTABLE} ${GIT_SAFE_DIRECTORY_ARGS} rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
                RESULT_VARIABLE GIT_DESCRIBE_ERROR_CODE
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        # Check if Dirty
        execute_process(
                COMMAND ${GIT_EXECUTABLE} ${GIT_SAFE_DIRECTORY_ARGS} diff -b --quiet --exit-code
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                RESULT_VARIABLE GIT_IS_DIRTY
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT GIT_DESCRIBE_ERROR_CODE)
            MESSAGE("Polaris Branch: ${GIT_DESCRIBE_BRANCH}")
            if(NOT GIT_DESCRIBE_BRANCH STREQUAL "master")
                set(PROJECT_VERSION ${PROJECT_VERSION}.${GIT_DESCRIBE_VERSION})
                MESSAGE("Polaris Version: ${GIT_DESCRIBE_VERSION}")
            endif()
            if(GIT_IS_DIRTY)
                set(PROJECT_VERSION ${PROJECT_VERSION}.dirty)
                MESSAGE("Git tree is dirty!")
            endif()
        else()
            MESSAGE(STATUS "Git metadata unavailable, using base project version")
        endif()
    else()
        MESSAGE(STATUS "Git metadata unavailable, using base project version")
    endif()
endif()

# A prerelease label from the release tag, beta.N or rc.N, or nothing for a stable build. It changes
# what the host reports and what each package format calls the build. PROJECT_VERSION stays the plain
# release number, because it also names the installed binary, /usr/bin/polaris-X.Y.Z, which the
# SteamOS namcap review pins, and the AppStream release.
set(POLARIS_PRERELEASE_LABEL "")
if(DEFINED ENV{POLARIS_PRERELEASE_LABEL})
    set(POLARIS_PRERELEASE_LABEL "$ENV{POLARIS_PRERELEASE_LABEL}")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/prerelease_versions.cmake)
polaris_prerelease_versions("${PROJECT_VERSION}" "${POLARIS_PRERELEASE_LABEL}")

# The date of the AppStream release entry: from a date-style version, else POLARIS_RELEASE_DATE, the
# HEAD commit date, or SOURCE_DATE_EPOCH (cmake/prep/release_date.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/release_date.cmake)
polaris_release_date("${PROJECT_VERSION}" "${CMAKE_SOURCE_DIR}")

# Parse PROJECT_VERSION to extract major, minor, and patch components
if(PROJECT_VERSION MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CMAKE_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")

    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CMAKE_PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")

    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(CMAKE_PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()

message("PROJECT_NAME: ${PROJECT_NAME}")
message("PROJECT_VERSION: ${PROJECT_VERSION}")
message("PROJECT_VERSION_MAJOR: ${PROJECT_VERSION_MAJOR}")
message("PROJECT_VERSION_MINOR: ${PROJECT_VERSION_MINOR}")
message("PROJECT_VERSION_PATCH: ${PROJECT_VERSION_PATCH}")
message("CMAKE_PROJECT_VERSION: ${CMAKE_PROJECT_VERSION}")
message("CMAKE_PROJECT_VERSION_MAJOR: ${CMAKE_PROJECT_VERSION_MAJOR}")
message("CMAKE_PROJECT_VERSION_MINOR: ${CMAKE_PROJECT_VERSION_MINOR}")
message("CMAKE_PROJECT_VERSION_PATCH: ${CMAKE_PROJECT_VERSION_PATCH}")
message("PROJECT_YEAR: ${PROJECT_YEAR}")
message("PROJECT_MONTH: ${PROJECT_MONTH}")
message("PROJECT_DAY: ${PROJECT_DAY}")
message("POLARIS_RELEASE_DATE_SOURCE: ${POLARIS_RELEASE_DATE_SOURCE}")
message("POLARIS_PRERELEASE_LABEL: ${POLARIS_PRERELEASE_LABEL}")
message("POLARIS_RUNTIME_VERSION: ${POLARIS_RUNTIME_VERSION}")
message("POLARIS_RPM_PACKAGE_VERSION: ${POLARIS_RPM_PACKAGE_VERSION}")
message("POLARIS_DEB_PACKAGE_VERSION: ${POLARIS_DEB_PACKAGE_VERSION}")
message("POLARIS_PACMAN_PKGVER: ${POLARIS_PACMAN_PKGVER}")

list(APPEND POLARIS_DEFINITIONS PROJECT_NAME="${PROJECT_NAME}")
# The C++ PROJECT_VERSION is the runtime version, label included: what /api/config, the update
# status, the capabilities reply, the log and crash reports all say this host is.
list(APPEND POLARIS_DEFINITIONS PROJECT_VERSION="${POLARIS_RUNTIME_VERSION}")
list(APPEND POLARIS_DEFINITIONS PROJECT_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}")
list(APPEND POLARIS_DEFINITIONS PROJECT_VERSION_MINOR="${PROJECT_VERSION_MINOR}")
list(APPEND POLARIS_DEFINITIONS PROJECT_VERSION_PATCH="${PROJECT_VERSION_PATCH}")
list(APPEND POLARIS_DEFINITIONS PROJECT_VERSION_COMMIT="${GITHUB_COMMIT}")
