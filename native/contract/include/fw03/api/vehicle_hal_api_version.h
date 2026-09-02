#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_error.h"

#include <cstdint>

#define FW03_VEHICLE_HAL_API_VERSION_MAJOR 1
#define FW03_VEHICLE_HAL_API_VERSION_MINOR 0
#define FW03_VEHICLE_HAL_API_VERSION_PATCH 1
#define FW03_VEHICLE_HAL_API_MIN_COMPATIBLE_MAJOR 1

namespace fw03::api {

struct ApiVersion final {
    std::uint16_t major{FW03_VEHICLE_HAL_API_VERSION_MAJOR};
    std::uint16_t minor{FW03_VEHICLE_HAL_API_VERSION_MINOR};
    std::uint16_t patch{FW03_VEHICLE_HAL_API_VERSION_PATCH};
    std::uint16_t min_compatible_major{FW03_VEHICLE_HAL_API_MIN_COMPATIBLE_MAJOR};
};

[[nodiscard]] constexpr ApiVersion CurrentApiVersion() noexcept { return ApiVersion{}; }

[[nodiscard]] bool operator==(const ApiVersion& lhs, const ApiVersion& rhs) noexcept;
[[nodiscard]] bool operator!=(const ApiVersion& lhs, const ApiVersion& rhs) noexcept;

[[nodiscard]] common::Result<ApiVersion, VehicleError> NegotiateApiVersion(
    const ApiVersion& client,
    const ApiVersion& service);

}  // namespace fw03::api
