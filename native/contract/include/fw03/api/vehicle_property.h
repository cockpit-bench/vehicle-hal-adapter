#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace fw03::api {

using RequestId = std::uint64_t;
using SessionId = std::uint64_t;
using SubscriberId = std::uint64_t;

struct PropertyKey final {
    std::uint32_t property_id{0U};
    std::uint32_t area_id{0U};
};

[[nodiscard]] bool operator==(const PropertyKey& lhs, const PropertyKey& rhs) noexcept;
[[nodiscard]] bool operator!=(const PropertyKey& lhs, const PropertyKey& rhs) noexcept;
[[nodiscard]] bool operator<(const PropertyKey& lhs, const PropertyKey& rhs) noexcept;

struct PropertyKeyHash final {
    [[nodiscard]] std::size_t operator()(const PropertyKey& key) const noexcept;
};

enum class PropertyStatus : std::uint8_t {
    kAvailable = 0,
    kUnavailable = 1,
    kError = 2,
};

using PropertyPayload = std::variant<
    bool,
    std::int32_t,
    std::int64_t,
    float,
    double,
    std::string,
    std::vector<std::uint8_t>>;

struct VehiclePropertyValue final {
    PropertyKey key;
    std::int64_t monotonic_timestamp_ns{0};
    PropertyStatus status{PropertyStatus::kAvailable};
    PropertyPayload payload{std::int32_t{0}};
};

[[nodiscard]] bool operator==(
    const VehiclePropertyValue& lhs,
    const VehiclePropertyValue& rhs) noexcept;
[[nodiscard]] bool operator!=(
    const VehiclePropertyValue& lhs,
    const VehiclePropertyValue& rhs) noexcept;

}  // namespace fw03::api
