set(FW03_PROTOC_EXECUTABLE "" CACHE FILEPATH "Path to the pinned protoc 25.3 executable")
if(FW03_PROTOC_EXECUTABLE STREQUAL "")
    find_program(FW03_PROTOC_EXECUTABLE NAMES protoc)
endif()
if(NOT FW03_PROTOC_EXECUTABLE)
    message(FATAL_ERROR "FW03 requires protoc 25.3 to compile and bind its canonical IPC descriptor")
endif()
execute_process(
    COMMAND "${FW03_PROTOC_EXECUTABLE}" --version
    RESULT_VARIABLE fw03_protoc_result
    OUTPUT_VARIABLE fw03_protoc_version
    ERROR_VARIABLE fw03_protoc_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT fw03_protoc_result EQUAL 0 OR NOT fw03_protoc_version STREQUAL "libprotoc 25.3")
    message(FATAL_ERROR
        "FW03 requires exact libprotoc 25.3; got '${fw03_protoc_version}' (${fw03_protoc_error})")
endif()

set(fw03_descriptor
    "${CMAKE_CURRENT_BINARY_DIR}/generated/vehicle_hal_contract.pb")
set(fw03_descriptor_header
    "${CMAKE_CURRENT_BINARY_DIR}/../generated/fw03/api/vehicle_hal_contract_descriptor.h")
# protoc does not create the parent of --descriptor_set_out.  A truly fresh
# build tree therefore has to create this output directory during configure;
# requiring a caller-side mkdir would make the documented CMake entry point
# non-reproducible.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${fw03_descriptor}" "${fw03_descriptor_header}"
    COMMAND "${FW03_PROTOC_EXECUTABLE}"
        -I "${CMAKE_CURRENT_SOURCE_DIR}"
        --include_imports
        "--descriptor_set_out=${fw03_descriptor}"
        "${CMAKE_CURRENT_SOURCE_DIR}/vehicle_hal_contract.proto"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${fw03_descriptor}"
        "-DOUTPUT=${fw03_descriptor_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EmbedDescriptor.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vehicle_hal_contract.proto"
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EmbedDescriptor.cmake"
    COMMENT "Generate and embed the canonical FW-03 protobuf descriptor with protoc 25.3"
    VERBATIM)

add_library(fw03_vehicle_contract STATIC
    src/vehicle_contract.cpp
    src/wire_codec.cpp
    "${fw03_descriptor_header}")

target_include_directories(
    fw03_vehicle_contract
    PUBLIC
        include
        "${CMAKE_CURRENT_BINARY_DIR}/../generated")
target_link_libraries(fw03_vehicle_contract PUBLIC fw03_common)
fw03_apply_warnings(fw03_vehicle_contract)
fw03_apply_runtime_checks(fw03_vehicle_contract)

add_custom_target(
    fw03_contract_descriptor ALL
    DEPENDS "${fw03_descriptor}" "${fw03_descriptor_header}")
