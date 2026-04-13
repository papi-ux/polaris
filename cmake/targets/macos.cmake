# macos specific target definitions
target_link_options(polaris PRIVATE LINKER:-sectcreate,__TEXT,__info_plist,${APPLE_PLIST_FILE})
# Tell linker to dynamically load these symbols at runtime, in case they're unavailable:
target_link_options(polaris PRIVATE -Wl,-U,_CGPreflightScreenCaptureAccess -Wl,-U,_CGRequestScreenCaptureAccess)
