#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_contract.h"
#include "fw03/hal/vehicle_hal_adapter.h"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace fw03::middleware {

using ValueResult = hal::RequestResult;
using ValueCompletion = hal::RequestCompletion;
using ClientEventCallback = std::function<void(api::PropertyEvent)>;
using GatewayStateCallback = std::function<void(bool, api::VehicleError)>;

class VehiclePropertyGateway final {
public:
    explicit VehiclePropertyGateway(hal::VehicleHalAdapter& hal_adapter);
    ~VehiclePropertyGateway();

    VehiclePropertyGateway(const VehiclePropertyGateway&) = delete;
    VehiclePropertyGateway& operator=(const VehiclePropertyGateway&) = delete;

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Start(
        const api::ApiVersion& requested_version = api::CurrentApiVersion());

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Get(
        api::PropertyKey key,
        std::chrono::milliseconds timeout,
        ValueCompletion completion,
        bool prefer_cache = true);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Set(
        api::VehiclePropertyValue value,
        std::chrono::milliseconds timeout,
        ValueCompletion completion);

    [[nodiscard]] common::Result<void, api::VehicleError> Subscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        float sample_rate_hz,
        ClientEventCallback callback);

    [[nodiscard]] common::Result<void, api::VehicleError> Unsubscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key);

    void SetStateCallback(GatewayStateCallback callback);
    void PollTimeouts();
    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Reconnect();
    [[nodiscard]] bool IsConnected() const noexcept;
    void Shutdown() noexcept;

private:
    struct CachedValue final {
        std::uint64_t sequence{0U};
        api::VehiclePropertyValue value;
    };

    void OnEvent(api::PropertyEvent event);
    void OnTransportState(bool connected, api::VehicleError error);

    hal::VehicleHalAdapter& hal_adapter_;
    mutable std::mutex mutex_;
    std::map<api::PropertyKey, CachedValue> cache_;
    std::map<api::SubscriberId, std::map<api::PropertyKey, ClientEventCallback>> callbacks_;
    GatewayStateCallback state_callback_;
    bool shutdown_{false};
};

}  // namespace fw03::middleware
