#pragma once

#include "fw03/common/clock.h"
#include "fw03/common/result.h"
#include "fw03/common/task_executor.h"
#include "fw03/api/vehicle_contract.h"
#include "fw03/hal/vehicle_subscription_registry.h"
#include "fw03/platform/vehicle_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
        RequestCompletion completion,
        api::SessionId owner_id = 0U);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Set(
        api::VehiclePropertyValue value,
        std::chrono::milliseconds timeout,
        RequestCompletion completion,
        api::SessionId owner_id = 0U);

    [[nodiscard]] common::Result<void, api::VehicleError> Subscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        float sample_rate_hz,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] common::Result<void, api::VehicleError> Unsubscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] common::Result<void, api::VehicleError> ReleaseSubscriber(
        api::SubscriberId subscriber_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    void CancelOwner(
        api::SessionId owner_id,
        api::VehicleError error) noexcept;

    void SetEventCallback(PropertyEventCallback callback);
    void SetTransportStateCallback(TransportStateCallback callback);

    void PollTimeouts();
    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Reconnect();
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] std::size_t DroppedEventCallbackCount() const noexcept;
    [[nodiscard]] std::size_t DroppedStaleEpochEventCount() const noexcept;
    [[nodiscard]] std::size_t DroppedStateCallbackCount() const noexcept;
    void Shutdown() noexcept;

private:
    struct CallbackEpochState;

    struct TransitionControl final {
        std::uint64_t expected_generation{0U};
        std::uint64_t obsolete_event_generation{0U};
        TransportStateCallback callback;
        api::VehicleError error;
        bool discard_obsolete_events{false};
        bool notify_state{false};
        bool expected_connected{false};
    };

    struct PendingRequest final {
        api::TransportOperation operation{api::TransportOperation::kGet};
        api::PropertyKey expected_key;
        api::SessionId owner_id{0U};
        common::Clock::TimePoint deadline;
        RequestCompletion completion;
        bool inline_completion{false};
    };

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Submit(
        api::TransportRequest request,
        std::chrono::milliseconds timeout,
        RequestCompletion completion,
        api::SessionId owner_id,
        bool inline_completion = false);
    [[nodiscard]] api::RequestId NextRequestId() noexcept;
    [[nodiscard]] common::Result<void, api::VehicleError> SendSubscriptionUpdate(
        api::PropertyKey key,
        const SubscriptionChange& change,
        std::chrono::milliseconds timeout);

    void OnResponse(api::TransportResponse response);
    void OnEvent(api::PropertyEvent event);
    void OnTransportDeath(api::VehicleError error);
    void Complete(RequestCompletion completion, bool inline_completion, RequestResult result);
    [[nodiscard]] bool QueueTransitionControlLocked(TransitionControl control);
    void DrainTransitionControls() noexcept;
    static bool BeginEventCallback(
        const std::shared_ptr<CallbackEpochState>& state,
        std::uint64_t epoch) noexcept;
    static void EndEventCallback(
        const std::shared_ptr<CallbackEpochState>& state) noexcept;
    static bool BeginStateCallback(
        const std::shared_ptr<CallbackEpochState>& state,
        std::uint64_t generation,
        bool expected_connected) noexcept;
    static void EndStateCallback(
        const std::shared_ptr<CallbackEpochState>& state) noexcept;
    static bool IsExecutingCallback(
        const std::shared_ptr<CallbackEpochState>& state) noexcept;

    const std::shared_ptr<platform::VehicleTransport> transport_;
    common::Clock& clock_;
    common::TaskExecutor& callback_executor_;
    const std::shared_ptr<CallbackEpochState> callback_epoch_state_;
    mutable std::mutex mutex_;
    std::mutex connect_attempt_mutex_;
    std::mutex subscription_update_mutex_;
    std::mutex transition_mutex_;
    std::condition_variable transition_changed_;
    std::deque<TransitionControl> transition_controls_;
    std::map<api::RequestId, PendingRequest> pending_;
    VehicleSubscriptionRegistry subscriptions_;
    PropertyEventCallback event_callback_;
    TransportStateCallback state_callback_;
    api::ApiVersion requested_version_{api::CurrentApiVersion()};
    std::atomic<api::RequestId> next_request_id_{1U};
    std::atomic<std::uint64_t> death_sequence_{0U};
    std::atomic<std::size_t> dropped_event_callbacks_{0U};
    std::atomic<std::size_t> dropped_state_callbacks_{0U};
    std::uint64_t handled_death_sequence_{0U};
    std::uint64_t connect_attempt_token_{0U};
    bool connect_attempt_active_{false};
    bool connect_attempt_invalidated_{false};
    bool transition_publisher_active_{false};
    bool connected_{false};
    bool shutdown_{false};
};

}  // namespace fw03::hal
