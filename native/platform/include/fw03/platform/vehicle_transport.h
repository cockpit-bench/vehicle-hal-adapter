#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_contract.h"

#include <functional>
#include <memory>
#include <string>

namespace fw03::platform {

struct TransportCallbacks final {
    std::function<void(api::TransportResponse)> on_response;
    std::function<void(api::PropertyEvent)> on_event;
    std::function<void(api::VehicleError)> on_death;
};

class VehicleTransport {
public:
    virtual ~VehicleTransport() = default;

    virtual void SetCallbacks(TransportCallbacks callbacks) = 0;
    [[nodiscard]] virtual common::Result<api::ApiVersion, api::VehicleError> Connect(
        const api::ApiVersion& requested_version) = 0;
    [[nodiscard]] virtual common::Result<void, api::VehicleError> Send(
        const api::TransportRequest& request) = 0;
    [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
    virtual void Shutdown() noexcept = 0;
};

[[nodiscard]] std::shared_ptr<VehicleTransport> CreateHostPosixVehicleTransport(
    std::string socket_path);

}  // namespace fw03::platform
