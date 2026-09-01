#pragma once

#include <cstdint>
#include <string>

namespace fw03::api {

enum class VehicleErrorCode : std::int32_t {
    kOk = 0,
    kInvalidArgument = 1,
    kNotSupported = 2,
    kPermissionDenied = 3,
    kTimeout = 4,
    kTransportDown = 5,
    kStaleValue = 6,
    kCancelled = 7,
    kInternal = 8,
    kIncompatibleVersion = 9,
};

struct VehicleError final {
    VehicleErrorCode code{VehicleErrorCode::kInternal};
    std::string detail;
    std::uint64_t request_id{0U};
};

[[nodiscard]] const char* ToString(VehicleErrorCode code) noexcept;

}  // namespace fw03::api
