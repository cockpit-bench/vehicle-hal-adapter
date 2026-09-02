include(FetchContent)

find_package(GTest 1.14.0 EXACT CONFIG QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gmock)
    message(FATAL_ERROR "GoogleTest 1.14.0 with GoogleMock is required for FW-03 integration tests")
endif()

add_library(fw03_test_support INTERFACE)
target_include_directories(fw03_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    fw03_test_support
    INTERFACE
        fw03_vehicle_service
        fw03_platform_adapter
        Threads::Threads
        GTest::gtest
        GTest::gmock)

add_executable(vehicle_service_integration_test vehicle_service_integration_test.cpp)
target_link_libraries(vehicle_service_integration_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(vehicle_service_integration_test)
fw03_apply_runtime_checks(vehicle_service_integration_test)

add_executable(vehicle_lifecycle_integration_test vehicle_lifecycle_integration_test.cpp)
target_link_libraries(vehicle_lifecycle_integration_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(vehicle_lifecycle_integration_test)
fw03_apply_runtime_checks(vehicle_lifecycle_integration_test)

add_executable(host_posix_transport_integration_test host_posix_transport_integration_test.cpp)
target_link_libraries(host_posix_transport_integration_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(host_posix_transport_integration_test)
fw03_apply_runtime_checks(host_posix_transport_integration_test)

add_executable(contract_common_integration_test contract_common_integration_test.cpp)
target_link_libraries(contract_common_integration_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(contract_common_integration_test)
fw03_apply_runtime_checks(contract_common_integration_test)

add_executable(client_ipc_vertical_integration_test client_ipc_vertical_integration_test.cpp)
target_link_libraries(client_ipc_vertical_integration_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(client_ipc_vertical_integration_test)
fw03_apply_runtime_checks(client_ipc_vertical_integration_test)

add_executable(daemon_options_test daemon_options_test.cpp)
target_link_libraries(daemon_options_test PRIVATE fw03_test_support GTest::gtest_main)
fw03_apply_warnings(daemon_options_test)
fw03_apply_runtime_checks(daemon_options_test)

get_filename_component(fw03_protoc_bin_dir "${FW03_PROTOC_EXECUTABLE}" DIRECTORY)
get_filename_component(fw03_protoc_root "${fw03_protoc_bin_dir}" DIRECTORY)
set(fw03_protoc_include_dir "${fw03_protoc_root}/include")
if(NOT EXISTS "${fw03_protoc_include_dir}/google/protobuf/descriptor.proto")
    message(FATAL_ERROR
        "FW-03 pinned protoc distribution is missing google/protobuf/descriptor.proto")
endif()

add_test(
    NAME contract_descriptor_schema_lock
    COMMAND "${CMAKE_COMMAND}"
        "-DPROTOC_EXECUTABLE=${FW03_PROTOC_EXECUTABLE}"
        "-DPROTOC_INCLUDE_DIR=${fw03_protoc_include_dir}"
        "-DPROTO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/../contract"
        "-DDESCRIPTOR=${CMAKE_CURRENT_BINARY_DIR}/../contract/generated/vehicle_hal_contract.pb"
        "-DBASELINE_SHA256_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../contract/vehicle_hal_contract.descriptor.sha256"
        "-DREPEAT_DESCRIPTOR=${CMAKE_CURRENT_BINARY_DIR}/vehicle_hal_contract.repeat.pb"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDescriptor.cmake")
set_tests_properties(contract_descriptor_schema_lock PROPERTIES TIMEOUT 10)

include(GoogleTest)
gtest_discover_tests(
    vehicle_service_integration_test
    TEST_PREFIX "service."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 10)
gtest_discover_tests(
    vehicle_lifecycle_integration_test
    TEST_PREFIX "lifecycle."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 10)
gtest_discover_tests(
    host_posix_transport_integration_test
    TEST_PREFIX "host_transport."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 10)
gtest_discover_tests(
    contract_common_integration_test
    TEST_PREFIX "contract_common."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 10)
gtest_discover_tests(
    client_ipc_vertical_integration_test
    TEST_PREFIX "client_ipc."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 20)
gtest_discover_tests(
    daemon_options_test
    TEST_PREFIX "daemon_options."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 10)
