if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "INPUT must name an existing descriptor set")
endif()
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT must name the generated descriptor header")
endif()

file(READ "${INPUT}" descriptor_hex HEX)
string(LENGTH "${descriptor_hex}" descriptor_hex_length)
if(descriptor_hex_length EQUAL 0)
    message(FATAL_ERROR "descriptor set is empty")
endif()
math(EXPR descriptor_size "${descriptor_hex_length} / 2")
math(EXPR descriptor_last "${descriptor_hex_length} - 2")

set(descriptor_bytes "")
set(column 0)
foreach(offset RANGE 0 ${descriptor_last} 2)
    string(SUBSTRING "${descriptor_hex}" ${offset} 2 byte)
    string(APPEND descriptor_bytes "0x${byte},")
    math(EXPR column "${column} + 1")
    if(column EQUAL 12)
        string(APPEND descriptor_bytes "\n    ")
        set(column 0)
    else()
        string(APPEND descriptor_bytes " ")
    endif()
endforeach()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}"
"#pragma once\n\n"
"#include <cstddef>\n"
"#include <cstdint>\n\n"
"namespace fw03::api::generated {\n\n"
"inline constexpr std::uint8_t kVehicleHalContractDescriptor[] = {\n"
"    ${descriptor_bytes}\n"
"};\n"
"inline constexpr std::size_t kVehicleHalContractDescriptorSize = ${descriptor_size}U;\n\n"
"}  // namespace fw03::api::generated\n")
