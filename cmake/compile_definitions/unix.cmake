# unix specific compile definitions
# put anything here that applies to both linux and macos

list(APPEND POLARIS_EXTERNAL_LIBRARIES
        ${CURL_LIBRARIES})

# add install prefix to assets path if not already there
if(NOT POLARIS_ASSETS_DIR MATCHES "^${CMAKE_INSTALL_PREFIX}")
    set(POLARIS_ASSETS_DIR "${CMAKE_INSTALL_PREFIX}/${POLARIS_ASSETS_DIR}")
endif()
