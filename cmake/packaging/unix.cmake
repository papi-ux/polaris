# unix specific packaging
# put anything here that applies to both linux and macos

# return here if building a macos package
if(POLARIS_PACKAGE_MACOS)
    return()
endif()

# Everything that does not say otherwise belongs to the main package. Only the DRM/KMS capture
# helper is packaged separately, and it says so, so this has to be set before the first install().
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "polaris")

# Installation destination dir
set(CPACK_SET_DESTDIR true)
if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "/usr/share/polaris")
endif()

install(TARGETS polaris RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
