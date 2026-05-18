# linux specific dependencies
include(CheckLibraryExists)

check_library_exists(va vaMapBuffer2 "" POLARIS_HAVE_LIBVA_MAP_BUFFER2)
if(NOT POLARIS_HAVE_LIBVA_MAP_BUFFER2)
    message(STATUS "libva does not export vaMapBuffer2; enabling compatibility shim")
endif()
