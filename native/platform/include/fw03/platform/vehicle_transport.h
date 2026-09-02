#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_contract.h"

#include <functional>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

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
        const api::TransportRequest& request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) = 0;
    [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
    virtual void Shutdown() noexcept = 0;
};

struct VehiclePeerTrustPolicy final {
    std::optional<std::uint32_t> expected_user_id;
    std::optional<std::uint32_t> expected_group_id;
    bool reject_world_writable_endpoint{true};
};

using VehicleTransportReaderThreadFactory =
    std::function<std::thread(std::function<void()>)>;

[[nodiscard]] std::shared_ptr<VehicleTransport> CreateHostPosixVehicleTransport(
    std::string socket_path,
    VehiclePeerTrustPolicy trust_policy = {},
    VehicleTransportReaderThreadFactory reader_thread_factory = {});

}  // namespace fw03::platform
