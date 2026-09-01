#include "fw03/api/vehicle_contract.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fw03::api {

bool operator==(const ApiVersion& lhs, const ApiVersion& rhs) noexcept {
    return lhs.major == rhs.major && lhs.minor == rhs.minor && lhs.patch == rhs.patch &&
           lhs.min_compatible_major == rhs.min_compatible_major;
}

bool operator!=(const ApiVersion& lhs, const ApiVersion& rhs) noexcept { return !(lhs == rhs); }

common::Result<ApiVersion, VehicleError> NegotiateApiVersion(
    const ApiVersion& client,
    const ApiVersion& service) {
    const bool major_matches = client.major == service.major;
    const bool service_accepts_client = client.major >= service.min_compatible_major;
    const bool client_accepts_service = service.major >= client.min_compatible_major;
    if (!major_matches || !service_accepts_client || !client_accepts_service) {
        return common::Result<ApiVersion, VehicleError>::Failure(
            {VehicleErrorCode::kIncompatibleVersion, "incompatible API major version", 0U});
    }

    return common::Result<ApiVersion, VehicleError>::Success(
        {service.major,
         std::min(client.minor, service.minor),
         std::min(client.patch, service.patch),
         std::max(client.min_compatible_major, service.min_compatible_major)});
}

const char* ToString(VehicleErrorCode code) noexcept {
    switch (code) {
        case VehicleErrorCode::kOk:
            return "OK";
        case VehicleErrorCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case VehicleErrorCode::kNotSupported:
            return "NOT_SUPPORTED";
        case VehicleErrorCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case VehicleErrorCode::kTimeout:
            return "TIMEOUT";
        case VehicleErrorCode::kTransportDown:
            return "TRANSPORT_DOWN";
        case VehicleErrorCode::kStaleValue:
            return "STALE_VALUE";
        case VehicleErrorCode::kCancelled:
            return "CANCELLED";
        case VehicleErrorCode::kInternal:
            return "INTERNAL";
        case VehicleErrorCode::kIncompatibleVersion:
            return "INCOMPATIBLE_VERSION";
    }
    return "INTERNAL";
}

bool operator==(const PropertyKey& lhs, const PropertyKey& rhs) noexcept {
    return lhs.property_id == rhs.property_id && lhs.area_id == rhs.area_id;
}

bool operator!=(const PropertyKey& lhs, const PropertyKey& rhs) noexcept { return !(lhs == rhs); }

bool operator<(const PropertyKey& lhs, const PropertyKey& rhs) noexcept {
    if (lhs.property_id != rhs.property_id) {
        return lhs.property_id < rhs.property_id;
    }
    return lhs.area_id < rhs.area_id;
}

std::size_t PropertyKeyHash::operator()(const PropertyKey& key) const noexcept {
    const auto property_hash = std::hash<std::uint32_t>{}(key.property_id);
    const auto area_hash = std::hash<std::uint32_t>{}(key.area_id);
    return property_hash ^ (area_hash + static_cast<std::size_t>(0x9e3779b9U) +
                            (property_hash << 6U) + (property_hash >> 2U));
}

bool operator==(const VehiclePropertyValue& lhs, const VehiclePropertyValue& rhs) noexcept {
    return lhs.key == rhs.key && lhs.monotonic_timestamp_ns == rhs.monotonic_timestamp_ns &&
           lhs.status == rhs.status && lhs.payload == rhs.payload;
}

bool operator!=(const VehiclePropertyValue& lhs, const VehiclePropertyValue& rhs) noexcept {
    return !(lhs == rhs);
}

common::Result<void, VehicleError> ValidateRequest(const TransportRequest& request) {
    if (request.request_id == 0U || request.key.property_id == 0U) {
        return common::Result<void, VehicleError>::Failure(
            {VehicleErrorCode::kInvalidArgument, "request and property IDs must be non-zero",
             request.request_id});
    }

    if (request.operation == TransportOperation::kSet) {
        if (!request.value.has_value() || request.value->key != request.key) {
            return common::Result<void, VehicleError>::Failure(
                {VehicleErrorCode::kInvalidArgument, "set value must match the request key",
                 request.request_id});
        }
    } else if (request.value.has_value()) {
        return common::Result<void, VehicleError>::Failure(
            {VehicleErrorCode::kInvalidArgument, "only set requests carry a property value",
             request.request_id});
    }

    if (request.operation == TransportOperation::kSubscribe &&
        (!std::isfinite(request.sample_rate_hz) || request.sample_rate_hz <= 0.0F ||
         request.sample_rate_hz > 100.0F)) {
        return common::Result<void, VehicleError>::Failure(
            {VehicleErrorCode::kInvalidArgument, "sample rate must be in (0, 100] Hz",
             request.request_id});
    }

    return common::Result<void, VehicleError>::Success();
}

}  // namespace fw03::api
