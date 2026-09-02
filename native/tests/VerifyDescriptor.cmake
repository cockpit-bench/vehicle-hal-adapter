foreach(required IN ITEMS PROTOC_EXECUTABLE PROTOC_INCLUDE_DIR PROTO_SOURCE DESCRIPTOR BASELINE_SHA256_FILE REPEAT_DESCRIPTOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "FW-03 descriptor verification requires ${required}")
    endif()
endforeach()

file(READ "${BASELINE_SHA256_FILE}" expected_sha256)
string(STRIP "${expected_sha256}" expected_sha256)
string(LENGTH "${expected_sha256}" expected_sha256_length)
if(NOT expected_sha256_length EQUAL 64 OR NOT expected_sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "FW-03 descriptor baseline is not a lowercase SHA-256")
endif()

file(SHA256 "${DESCRIPTOR}" descriptor_sha256)
if(NOT descriptor_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
        "FW-03 descriptor changed without an explicitly reviewed schema baseline update: "
        "expected ${expected_sha256}, got ${descriptor_sha256}")
endif()

execute_process(
    COMMAND "${PROTOC_EXECUTABLE}"
        -I "${PROTO_SOURCE}"
        --include_imports
        "--descriptor_set_out=${REPEAT_DESCRIPTOR}"
        "${PROTO_SOURCE}/vehicle_hal_contract.proto"
    RESULT_VARIABLE repeat_result
    ERROR_VARIABLE repeat_error)
if(NOT repeat_result EQUAL 0)
    message(FATAL_ERROR "FW-03 repeat descriptor generation failed: ${repeat_error}")
endif()
file(SHA256 "${REPEAT_DESCRIPTOR}" repeat_sha256)
if(NOT repeat_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
        "FW-03 descriptor generation is not byte deterministic: "
        "expected ${expected_sha256}, got ${repeat_sha256}")
endif()

execute_process(
    COMMAND "${PROTOC_EXECUTABLE}"
        --decode=google.protobuf.FileDescriptorSet
        "--proto_path=${PROTOC_INCLUDE_DIR}"
        google/protobuf/descriptor.proto
    INPUT_FILE "${DESCRIPTOR}"
    OUTPUT_VARIABLE decoded_descriptor
    ERROR_VARIABLE decode_error
    RESULT_VARIABLE decode_result)
if(NOT decode_result EQUAL 0)
    message(FATAL_ERROR "FW-03 descriptor decode failed: ${decode_error}")
endif()

foreach(required_marker IN ITEMS
        "name: \"WireEnvelope\""
        "name: \"TransportOperation\""
        "name: \"VehicleError\""
        "name: \"VehicleWireContract\""
        "name: \"Exchange\""
        "client_streaming: true"
        "server_streaming: true")
    string(FIND "${decoded_descriptor}" "${required_marker}" marker_offset)
    if(marker_offset EQUAL -1)
        message(FATAL_ERROR "FW-03 descriptor is missing ${required_marker}")
    endif()
endforeach()
