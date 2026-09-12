# Apply the reviewed backport to a build-tree copy. The canonical submodule is
# unchanged, so package archives and ordinary submodule checkout stay portable.
function(polaris_prepare_inputtino source output)
    set(paths
        include/inputtino/input.hpp
        src/uhid/include/uhid/protected_types.hpp
        src/uhid/include/uhid/uhid.hpp
        src/uhid/include/uhid/ps5.hpp
        src/uhid/joypad_ps5.cpp
        src/uinput/include/inputtino/protected_types.hpp
        src/uinput/keyboard.cpp
        src/uinput/mouse.cpp
        src/uinput/touchscreen.cpp
        src/uinput/pentablet.cpp
        src/uinput/trackpad.cpp
        src/uinput/joypad_xbox.cpp
        src/uinput/joypad_nintendo.cpp
        src/uinput/joypad_ps.cpp)
    set(hashes
        6643a5fd9ff101e9451398bf9cd0f7eb051bd2f10f294e1060a93fa6f3d0ca08
        2f8de358a4c0f353d97b165afc7f902d10c2ed01929f09e7a21dd2b2c8fd0f22
        95e6c94c1343e804866a49c9ef666f3c111b75144eecc02c378c1b88efc1fda5
        39bc96b1b30ee96c463403a006d954658277abdfcd3bca8f28893ae5426b96d1
        1a4445915d5ef58115a588f013472e42fcdbc91eb70e08fb22adbb93db316d52
        f8cfbea8d3e46a596edb28431ef159b2f9d4077685a47f912b727ac4ea24a4d2
        f91735f3c5071cee64a2ca0013f650f54c1011270b5d5af97542d644330da802
        1c4bedca432c2a6fe220517768941fd2ab269f1664a14aaac1dadb4e86a2a3bf
        ea1413d9e2d615f28c7f2812d799224b9d00c3b993b6d041c7c59d55e3c41519
        ca6350652dcf2c75497dc44b84bc1e9c312941bac240edfe63134544c9b7d55e
        12df2dd58c4808d45c2863de54964546f3499b7fcfe31986a5a62703f161f077
        d70ac202c8378a4a73cb47b4cf32b9990dfee2bd8d7c9504e42580a92e9c18db
        815486fd69e75dc32c544e953aff9d94a84e3f585a6d1f5fe8947837f271a456
        e162aee6a115944136bbd7ea4aeca260f651a4b4bb55d49e2b982eb84b6aa7cd)
    list(LENGTH paths input_count)
    math(EXPR last_input "${input_count} - 1")
    foreach(index RANGE 0 ${last_input})
        list(GET paths ${index} path)
        list(GET hashes ${index} expected)
        file(SHA256 "${source}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "inputtino backport input changed: ${path}; review the canonical pin and patch")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}/${path}")
    endforeach()
    find_program(POLARIS_PATCH_EXECUTABLE patch REQUIRED)
    file(COPY "${source}/" DESTINATION "${output}" PATTERN ".git" EXCLUDE)
    foreach(name
            0001-serialize-dualsense-reports-and-own-threads.patch
            0002-propagate-uinput-physical-identity.patch
            0003-neutral-dualsense-resting-axes.patch)
        set(backport "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../packaging/linux/patches/inputtino/${name}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${backport}")
        execute_process(
            COMMAND "${POLARIS_PATCH_EXECUTABLE}" -p1 --batch --forward --fuzz=0 -i "${backport}"
            WORKING_DIRECTORY "${output}"
            RESULT_VARIABLE result OUTPUT_VARIABLE patch_output ERROR_VARIABLE patch_error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "inputtino backport failed: ${patch_output}${patch_error}")
        endif()
    endforeach()
endfunction()
