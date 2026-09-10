# Apply the reviewed backport to a build-tree copy. The canonical submodule is
# unchanged, so package archives and ordinary submodule checkout stay portable.
function(polaris_prepare_inputtino source output)
    set(paths
        include/inputtino/input.hpp
        src/uhid/include/uhid/protected_types.hpp
        src/uhid/include/uhid/uhid.hpp
        src/uhid/joypad_ps5.cpp)
    set(hashes
        6643a5fd9ff101e9451398bf9cd0f7eb051bd2f10f294e1060a93fa6f3d0ca08
        2f8de358a4c0f353d97b165afc7f902d10c2ed01929f09e7a21dd2b2c8fd0f22
        95e6c94c1343e804866a49c9ef666f3c111b75144eecc02c378c1b88efc1fda5
        1a4445915d5ef58115a588f013472e42fcdbc91eb70e08fb22adbb93db316d52)
    foreach(index RANGE 0 3)
        list(GET paths ${index} path)
        list(GET hashes ${index} expected)
        file(SHA256 "${source}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "inputtino backport input changed: ${path}; review the canonical pin and patch")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}/${path}")
    endforeach()
    set(backport "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../packaging/linux/patches/inputtino/0001-serialize-dualsense-reports-and-own-threads.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${backport}")
    find_program(POLARIS_PATCH_EXECUTABLE patch REQUIRED)
    file(COPY "${source}/" DESTINATION "${output}" PATTERN ".git" EXCLUDE)
    execute_process(
        COMMAND "${POLARIS_PATCH_EXECUTABLE}" -p1 --batch --forward --fuzz=0 -i "${backport}"
        WORKING_DIRECTORY "${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE patch_output ERROR_VARIABLE patch_error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "inputtino backport failed: ${patch_output}${patch_error}")
    endif()
endfunction()
