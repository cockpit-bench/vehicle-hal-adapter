if(NOT FW_PLATFORM STREQUAL "host_posix")
    message(FATAL_ERROR "No target transport is compiled without its platform SDK")
endif()

add_library(fw03_platform_adapter STATIC
    src/host_posix_vehicle_transport.cpp)

target_include_directories(fw03_platform_adapter PUBLIC include)
target_link_libraries(fw03_platform_adapter PUBLIC fw03_vehicle_contract Threads::Threads)
fw03_apply_warnings(fw03_platform_adapter)
fw03_apply_runtime_checks(fw03_platform_adapter)
