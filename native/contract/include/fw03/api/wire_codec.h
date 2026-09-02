#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_contract.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace fw03::api {

inline constexpr std::uint32_t kVehicleWireMagic = 0x46573033U;
inline constexpr std::uint16_t kVehicleWireMajor = 1U;
inline constexpr std::uint16_t kVehicleWireMinor = 0U;

enum class WireMessageKind : std::uint8_t {
    kHello = 1U,
    kHelloAck = 2U,
    kRequest = 3U,
    kResponse = 4U,
    kEvent = 5U,
};

struct Hello final {
    ApiVersion requested_version;
};

struct HelloAck final {
    VehicleError error{VehicleErrorCode::kOk, {}, 0U};
    ApiVersion negotiated_version;
};

using WireMessage = std::variant<Hello, HelloAck, TransportRequest, TransportResponse, PropertyEvent>;

[[nodiscard]] common::Result<std::vector<std::uint8_t>, VehicleError> EncodeWireMessage(
    const WireMessage& message);

[[nodiscard]] common::Result<WireMessage, VehicleError> DecodeWireMessage(
    const std::vector<std::uint8_t>& bytes);

}  // namespace fw03::api
