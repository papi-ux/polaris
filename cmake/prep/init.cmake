if (WIN32)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15.0)
            add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-template-body>) # Workaround for WinRT headers
        endif()
    endif()
elseif (APPLE)
elseif (UNIX)
    include(GNUInstallDirs)

    if(NOT DEFINED POLARIS_EXECUTABLE_PATH)
        set(POLARIS_EXECUTABLE_PATH "polaris")
    endif()

    set(POLARIS_SERVICE_START_COMMAND "ExecStart=${POLARIS_EXECUTABLE_PATH}")
    set(POLARIS_SERVICE_STOP_COMMAND "")
endif()
