#pragma once

#include "fw03/api/vehicle_error.h"
#include "fw03/api/vehicle_hal_api_version.h"
#include "fw03/api/vehicle_property.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fw03::api {

enum class TransportOperation : std::uint8_t {
    kGet = 1,
    kSet = 2,
    kSubscribe = 3,
    kUnsubscribe = 4,
};

struct TransportRequest final {
    RequestId request_id{0U};
    TransportOperation operation{TransportOperation::kGet};
    PropertyKey key;
    float sample_rate_hz{0.0F};
    std::optional<VehiclePropertyValue> value;
};

struct TransportResponse final {
    RequestId request_id{0U};
    VehicleError error{VehicleErrorCode::kOk, {}, 0U};
    std::optional<VehiclePropertyValue> value;
};

struct PropertyEvent final {
    std::uint64_t sequence{0U};
    VehiclePropertyValue value;
};

[[nodiscard]] common::Result<void, VehicleError> ValidateRequest(
    const TransportRequest& request);

}  // namespace fw03::api
