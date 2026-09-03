function(fw03_validate_platform platform)
    set(valid_platforms host_posix android_ndk)
    if(NOT platform IN_LIST valid_platforms)
        message(FATAL_ERROR "Unsupported FW_PLATFORM='${platform}'. Expected one of: ${valid_platforms}")
    endif()
    if(NOT platform STREQUAL "host_posix")
        message(FATAL_ERROR
            "FW_PLATFORM='${platform}' requires a legally available target SDK and an implemented "
            "adapter. This source slice intentionally refuses to build an empty target stub.")
    endif()
endfunction()
