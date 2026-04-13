# unix specific packaging
# put anything here that applies to both linux and macos

# return here if building a macos package
if(POLARIS_PACKAGE_MACOS)
    return()
endif()

# Installation destination dir
set(CPACK_SET_DESTDIR true)
if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "/usr/share/polaris")
endif()

install(TARGETS polaris RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
