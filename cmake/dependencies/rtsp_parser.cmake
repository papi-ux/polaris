# moonlight-common-c c999436858471dfefa7617af3b7dc03ec1644ce4.
# Keep the canonical parser and its headers untouched. Only the parser-owned
# duplicate node is released in this build-tree copy; insertOption stays generic.
function(polaris_prepare_rtsp_parser source output)
    set(paths RtspParser.c Rtsp.h Platform.h Limelight.h)
    set(hashes
        ba6e39ee53c0ba93eaf0083c0b2c24aae2cc1610c4ebda82ad658004615ee049
        f6373aad91951d6c5155c826b23a28fa40e6bff3e62304ef088b12b1cb44e253
        231d7543010a554fc5d2b0e6a3dc879a7d174bbd53e0b3848d1b8e45f9c7b9e4
        872c18f713d09c49710776fde96c5b19e2334e07f4b2c04992b622604e57a4e4)
    foreach(index RANGE 0 3)
        list(GET paths ${index} path)
        list(GET hashes ${index} expected)
        if(NOT EXISTS "${source}/${path}")
            message(FATAL_ERROR "Missing pinned moonlight-common-c input: ${path}")
        endif()
        file(SHA256 "${source}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "RTSP parser input changed: ${path}; review the ownership correction")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}/${path}")
    endforeach()

    file(READ "${source}/RtspParser.c" content)
    set(before [=[                insertOption(&options, newOpt);
]=])
    set(after [=[                insertOption(&options, newOpt);
                // A duplicate updates the first node but leaves newOpt unlinked.
                // The parser owns this node; its strings belong to messageBuffer.
                {
                    POPTION_ITEM linkedOpt = options;
                    while (linkedOpt != NULL && linkedOpt != newOpt) {
                        linkedOpt = linkedOpt->next;
                    }
                    if (linkedOpt == NULL) {
                        free(newOpt);
                    }
                }
]=])
    string(REPLACE "${before}" "${after}" adapted "${content}")
    if(adapted STREQUAL content)
        message(FATAL_ERROR "Pinned RTSP parser insertion site not found")
    endif()

    file(MAKE_DIRECTORY "${output}")
    # Preserve the original quoted-include closure, rather than resolving a
    # project header with the same name from the copied parser's new location.
    foreach(header Rtsp.h Platform.h Limelight.h)
        configure_file("${source}/${header}" "${output}/${header}" COPYONLY)
    endforeach()
    set(destination "${output}/RtspParser.c")
    set(previous "")
    if(EXISTS "${destination}")
        file(READ "${destination}" previous)
    endif()
    if(NOT previous STREQUAL adapted)
        file(WRITE "${destination}" "${adapted}")
    endif()
endfunction()
