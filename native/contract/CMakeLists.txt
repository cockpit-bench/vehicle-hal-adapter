add_library(fw03_vehicle_contract STATIC
    src/vehicle_contract.cpp
    src/wire_codec.cpp)

target_include_directories(
    fw03_vehicle_contract
    PUBLIC
        include
        "${CMAKE_CURRENT_BINARY_DIR}/../generated")
target_link_libraries(fw03_vehicle_contract PUBLIC fw03_common)
fw03_apply_warnings(fw03_vehicle_contract)
fw03_apply_runtime_checks(fw03_vehicle_contract)

add_custom_target(
    fw03_contract_fingerprint
    COMMAND "${CMAKE_COMMAND}" -E sha256sum "${CMAKE_CURRENT_SOURCE_DIR}/vehicle_hal_contract.proto"
    COMMENT "Reproduce the vehicle HAL contract SHA-256")
