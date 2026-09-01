#pragma once

#include "fw03/common/clock.h"
#include "fw03/common/result.h"
#include "fw03/common/task_executor.h"
#include "fw03/api/vehicle_contract.h"
#include "fw03/hal/vehicle_subscription_registry.h"
#include "fw03/platform/vehicle_transport.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fw03::hal {

using RequestResult = common::Result<std::optional<api::VehiclePropertyValue>, api::VehicleError>;
using RequestCompletion = std::function<void(RequestResult)>;
using PropertyEventCallback = std::function<void(api::PropertyEvent)>;
using TransportStateCallback = std::function<void(bool, api::VehicleError)>;

class VehicleHalAdapter final {
public:
    VehicleHalAdapter(
        std::shared_ptr<platform::VehicleTransport> transport,
        common::Clock& clock,
        common::TaskExecutor& callback_executor);
    ~VehicleHalAdapter();

    VehicleHalAdapter(const VehicleHalAdapter&) = delete;
    VehicleHalAdapter& operator=(const VehicleHalAdapter&) = delete;

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Start(
        const api::ApiVersion& requested_version = api::CurrentApiVersion());

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Get(
        api::PropertyKey key,
        std::chrono::milliseconds timeout,
        RequestCompletion completion);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Set(
        api::VehiclePropertyValue value,
        std::chrono::milliseconds timeout,
        RequestCompletion completion);

    [[nodiscard]] common::Result<void, api::VehicleError> Subscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        float sample_rate_hz);

    [[nodiscard]] common::Result<void, api::VehicleError> Unsubscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key);

    void SetEventCallback(PropertyEventCallback callback);
    void SetTransportStateCallback(TransportStateCallback callback);

    void PollTimeouts();
    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Reconnect();
    [[nodiscard]] bool IsConnected() const noexcept;
    void Shutdown() noexcept;

private:
    struct PendingRequest final {
        api::TransportOperation operation{api::TransportOperation::kGet};
        common::Clock::TimePoint deadline;
        RequestCompletion completion;
    };

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Submit(
        api::TransportRequest request,
        std::chrono::milliseconds timeout,
        RequestCompletion completion);
    [[nodiscard]] api::RequestId NextRequestId() noexcept;
    [[nodiscard]] common::Result<void, api::VehicleError> SendSubscriptionUpdate(
        api::PropertyKey key,
        const SubscriptionChange& change);

    void OnResponse(api::TransportResponse response);
    void OnEvent(api::PropertyEvent event);
    void OnTransportDeath(api::VehicleError error);
    void Complete(RequestCompletion completion, RequestResult result);

    const std::shared_ptr<platform::VehicleTransport> transport_;
    common::Clock& clock_;
    common::TaskExecutor& callback_executor_;
    mutable std::mutex mutex_;
    std::mutex subscription_update_mutex_;
    std::map<api::RequestId, PendingRequest> pending_;
    VehicleSubscriptionRegistry subscriptions_;
    PropertyEventCallback event_callback_;
    TransportStateCallback state_callback_;
    api::ApiVersion requested_version_{api::CurrentApiVersion()};
    std::atomic<api::RequestId> next_request_id_{1U};
    bool connected_{false};
    bool shutdown_{false};
};

}  // namespace fw03::hal
