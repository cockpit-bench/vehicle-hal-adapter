cmake_minimum_required(VERSION 3.25)

project(
    fw03_vehicle_hal_adapter
    VERSION 1.0.0
    DESCRIPTION "Versioned embedded vehicle-property HAL adapter"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FW03_WARNINGS_AS_ERRORS "Treat production and test warnings as errors" ON)
option(ENABLE_ASAN "Enable AddressSanitizer for host validation" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for host validation" OFF)
option(ENABLE_COVERAGE "Enable GCC/Clang source coverage" OFF)
set(FW_PLATFORM "host_posix" CACHE STRING "Platform adapter: host_posix, android_ndk, or qnx")
set_property(CACHE FW_PLATFORM PROPERTY STRINGS host_posix android_ndk qnx)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(PlatformSelection)

fw03_validate_platform("${FW_PLATFORM}")
find_package(Threads REQUIRED)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/contract/include/fw03/api/build_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/fw03/api/build_version.h"
    @ONLY)

# Production ownership graph. Tests are deliberately outside this six-module count.
add_subdirectory(common)
add_subdirectory(contract)
add_subdirectory(platform)
add_subdirectory(hal)
add_subdirectory(middleware)
add_subdirectory(application)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
