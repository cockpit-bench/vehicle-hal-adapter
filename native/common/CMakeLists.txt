add_library(fw03_common STATIC
    src/serial_executor.cpp)

target_include_directories(fw03_common PUBLIC include)
target_link_libraries(fw03_common PUBLIC Threads::Threads)
fw03_apply_warnings(fw03_common)
fw03_apply_runtime_checks(fw03_common)
