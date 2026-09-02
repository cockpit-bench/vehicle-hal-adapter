add_library(fw03_vehicle_hal STATIC
    src/vehicle_hal_adapter.cpp
    src/vehicle_subscription_registry.cpp)

target_include_directories(fw03_vehicle_hal PUBLIC include)
target_link_libraries(fw03_vehicle_hal PUBLIC fw03_common fw03_vehicle_contract fw03_platform_adapter)
fw03_apply_warnings(fw03_vehicle_hal)
fw03_apply_runtime_checks(fw03_vehicle_hal)
