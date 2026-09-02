#include "fw03/application/daemon_options.h"
#include "fw03/application/platform_profile.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace fw03::test {
namespace {

common::Result<application::DaemonOptions, std::string> Parse(
    std::initializer_list<const char*> arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for (const auto* argument : arguments) {
        storage.emplace_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& argument : storage) {
        argv.push_back(argument.data());
    }
    return application::ParseDaemonOptions(
        static_cast<int>(argv.size()),
        argv.data());
}

TEST(DaemonOptionsTest, ProvidesAUsableConfiguredProfileByDefault) {
    const auto parsed = Parse({"vehicle_gateway_daemon"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed.value().hal_socket, application::profile::kDefaultHalSocket);
    EXPECT_EQ(parsed.value().client_socket, application::profile::kDefaultClientSocket);
    const std::string profile_name = application::profile::kName;
    if (profile_name == "generic") {
        EXPECT_EQ(parsed.value().hal_socket, "/tmp/fw03-vehicle-hal.sock");
    } else if (profile_name == "sa8155") {
        EXPECT_EQ(parsed.value().hal_socket, "/run/vehicle/sa8155-vhal.sock");
    } else if (profile_name == "sa8295") {
        EXPECT_EQ(parsed.value().hal_socket, "/run/vehicle/sa8295-vhal.sock");
    } else {
        FAIL() << "unexpected compiled FW-03 profile " << profile_name;
    }
    EXPECT_FALSE(parsed.value().expected_hal_user_id.has_value());
    EXPECT_FALSE(parsed.value().expected_hal_group_id.has_value());
    EXPECT_TRUE(parsed.value().allowed_client_user_ids.empty());
    EXPECT_TRUE(parsed.value().allowed_client_group_ids.empty());
    EXPECT_EQ(parsed.value().readable_properties.count({0x11600207U, 0U}), 1U);
    EXPECT_EQ(parsed.value().writable_properties.count({0x15400500U, 1U}), 1U);
}

TEST(DaemonOptionsTest, AcceptsExplicitEndpointsLegacyAliasAndPropertyIds) {
    const auto parsed = Parse({
        "vehicle_gateway_daemon",
        "--socket",
        "/tmp/downstream.sock",
        "--client-socket",
        "/tmp/upstream.sock",
        "--hal-uid",
        "0",
        "--hal-gid",
        "10",
        "--client-uid",
        "1000",
        "--client-gid",
        "2000",
        "--read-property",
        "0x1234:7",
        "--write-property",
        "4661:2"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed.value().hal_socket, "/tmp/downstream.sock");
    EXPECT_EQ(parsed.value().client_socket, "/tmp/upstream.sock");
    ASSERT_TRUE(parsed.value().expected_hal_user_id.has_value());
    EXPECT_EQ(*parsed.value().expected_hal_user_id, 0U);
    ASSERT_TRUE(parsed.value().expected_hal_group_id.has_value());
    EXPECT_EQ(*parsed.value().expected_hal_group_id, 10U);
    EXPECT_EQ(parsed.value().allowed_client_user_ids.count(1000U), 1U);
    EXPECT_EQ(parsed.value().allowed_client_group_ids.count(2000U), 1U);
    EXPECT_EQ(parsed.value().readable_properties.count({0x1234U, 7U}), 1U);
    EXPECT_EQ(parsed.value().writable_properties.count({4661U, 2U}), 1U);
}

TEST(DaemonOptionsTest, RejectsMissingUnknownAndInvalidArguments) {
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--hal-socket"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--unknown"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--read-property", "0"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--read-property", "12tail"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--read-property", "4294967296"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--write-property", "not-a-number"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--read-property", "7:"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--read-property", "7:4294967296"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--client-uid", "-1"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--hal-gid", "-1"}));
}

TEST(DaemonOptionsTest, RequiresDistinctNonEmptySocketPaths) {
    EXPECT_FALSE(Parse({
        "vehicle_gateway_daemon",
        "--hal-socket",
        "/tmp/same.sock",
        "--client-socket",
        "/tmp/same.sock"}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--hal-socket", ""}));
    EXPECT_FALSE(Parse({"vehicle_gateway_daemon", "--client-socket", ""}));
    EXPECT_FALSE(application::ParseDaemonOptions(0, nullptr));
    EXPECT_NE(std::string(application::DaemonUsage()).find("--client-socket"), std::string::npos);
}

}  // namespace
}  // namespace fw03::test
