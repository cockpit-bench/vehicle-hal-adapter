#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_property.h"

#include <cstdint>
#include <set>
#include <optional>
#include <string>

namespace fw03::application {

struct DaemonOptions final {
    std::string hal_socket;
    std::string client_socket;
    std::optional<std::uint32_t> expected_hal_user_id;
    std::optional<std::uint32_t> expected_hal_group_id;
    std::set<std::uint32_t> allowed_client_user_ids;
    std::set<std::uint32_t> allowed_client_group_ids;
    std::set<api::PropertyKey> readable_properties;
    std::set<api::PropertyKey> writable_properties;
};

[[nodiscard]] common::Result<DaemonOptions, std::string> ParseDaemonOptions(
    int argc,
    char* const argv[]);

[[nodiscard]] const char* DaemonUsage() noexcept;

}  // namespace fw03::application
