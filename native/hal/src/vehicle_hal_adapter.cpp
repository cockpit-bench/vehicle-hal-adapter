#include "fw03/hal/vehicle_hal_adapter.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <utility>

namespace fw03::hal {
namespace {

constexpr std::size_t kMaximumPendingRequests = 256U;

thread_local std::vector<const void*> g_callback_state_stack;

std::uint64_t EventCoalescingKey(api::PropertyKey key, std::uint64_t epoch) noexcept {
    const auto property_key = (static_cast<std::uint64_t>(key.property_id) << 32U) |
                              static_cast<std::uint64_t>(key.area_id);
    const auto epoch_mix = epoch * UINT64_C(0x9e3779b97f4a7c15);
    return property_key ^ epoch_mix ^ (property_key << 7U) ^ (property_key >> 3U);
}

}  // namespace

struct VehicleHalAdapter::CallbackEpochState final {
    std::mutex mutex;
    std::condition_variable idle;
    std::uint64_t epoch{0U};
    std::size_t active_event_callbacks{0U};
    std::size_t active_callbacks{0U};
    std::atomic<std::size_t> dropped_stale_epoch_events{0U};
    std::atomic<std::size_t> dropped_stale_state_callbacks{0U};
    bool connected{false};
    bool alive{true};
};

VehicleHalAdapter::VehicleHalAdapter(
    std::shared_ptr<platform::VehicleTransport> transport,
    common::Clock& clock,
    common::TaskExecutor& callback_executor)
    : transport_(std::move(transport)),
      clock_(clock),
      callback_executor_(callback_executor),
      callback_epoch_state_(std::make_shared<CallbackEpochState>()) {
    transport_->SetCallbacks({
        [this](api::TransportResponse response) { OnResponse(std::move(response)); },
        [this](api::PropertyEvent event) { OnEvent(std::move(event)); },
        [this](api::VehicleError error) { OnTransportDeath(std::move(error)); },
    });
}

VehicleHalAdapter::~VehicleHalAdapter() { Shutdown(); }

common::Result<api::ApiVersion, api::VehicleError> VehicleHalAdapter::Start(
    const api::ApiVersion& requested_version) {
    if (IsExecutingCallback(callback_epoch_state_)) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInternal,
             "connection start cannot run reentrantly from an adapter callback", 0U});
    }
    std::lock_guard<std::mutex> attempt_lock(connect_attempt_mutex_);
    std::uint64_t attempt_token = 0U;
    {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load() &&
                   !transition_publisher_active_ && transition_controls_.empty() &&
                   !connect_attempt_active_;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "HAL adapter is shut down", 0U});
        }
        requested_version_ = requested_version;
        connect_attempt_active_ = true;
        connect_attempt_invalidated_ = false;
        attempt_token = ++connect_attempt_token_;
    }

    const auto connected_version = transport_->Connect(requested_version);
    const bool transport_connected = connected_version.ok() && transport_->IsConnected();
    bool committed = false;
    bool publish_controls = false;
    bool cancelled = false;
    {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load();
        });
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled = shutdown_;
        if (!shutdown_ && connect_attempt_active_ &&
            connect_attempt_token_ == attempt_token && !connect_attempt_invalidated_ &&
            transport_connected) {
            std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
            const auto obsolete_generation = callback_epoch_state_->epoch;
            connected_ = true;
            ++callback_epoch_state_->epoch;
            callback_epoch_state_->connected = true;
            publish_controls = QueueTransitionControlLocked(
                {callback_epoch_state_->epoch,
                 obsolete_generation,
                 {},
                 {},
                 true,
                 false,
                 true});
            committed = true;
        }
        connect_attempt_active_ = false;
        transition_changed_.notify_all();
    }
    if (publish_controls) {
        DrainTransitionControls();
    }
    if (committed) {
        return connected_version;
    }
    if (cancelled) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kCancelled, "HAL adapter was shut down during connect", 0U});
    }
    if (!connected_version) {
        return connected_version;
    }
    return common::Result<api::ApiVersion, api::VehicleError>::Failure(
        {api::VehicleErrorCode::kTransportDown,
         "vehicle transport died before the connection could be committed", 0U});
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Get(
    api::PropertyKey key,
    std::chrono::milliseconds timeout,
    RequestCompletion completion,
    api::SessionId owner_id) {
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = api::TransportOperation::kGet;
    request.key = key;
    return Submit(std::move(request), timeout, std::move(completion), owner_id);
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Set(
    api::VehiclePropertyValue value,
    std::chrono::milliseconds timeout,
    RequestCompletion completion,
    api::SessionId owner_id) {
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = api::TransportOperation::kSet;
    request.key = value.key;
    request.value = std::move(value);
    return Submit(std::move(request), timeout, std::move(completion), owner_id);
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Submit(
    api::TransportRequest request,
    std::chrono::milliseconds timeout,
    RequestCompletion completion,
    api::SessionId owner_id,
    bool inline_completion) {
    if (!completion || timeout <= std::chrono::milliseconds::zero()) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "request completion and positive timeout are required", request.request_id});
    }
    const auto validation = api::ValidateRequest(request);
    if (!validation) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(validation.error());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_ || !connected_) {
            return common::Result<api::RequestId, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                  "vehicle transport is not connected", request.request_id});
        }
        if (pending_.size() >= kMaximumPendingRequests) {
            return common::Result<api::RequestId, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal,
                 "vehicle request capacity is exhausted", request.request_id});
        }
        pending_.emplace(
            request.request_id,
            PendingRequest{
                request.operation,
                request.key,
                owner_id,
                clock_.Now() + timeout,
                std::move(completion),
                inline_completion});
    }

    const auto sent = transport_->Send(request, timeout);
    if (!sent) {
        bool submit_still_owned_pending = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pending = pending_.find(request.request_id);
            if (pending != pending_.end()) {
                pending_.erase(pending);
                submit_still_owned_pending = true;
            }
        }
        if (submit_still_owned_pending) {
            // Immediate rejection means the request was never accepted and therefore has no
            // completion.  If transport death already claimed the pending entry, return accepted
            // so that its single completion remains the only settlement visible to the caller.
            return common::Result<api::RequestId, api::VehicleError>::Failure(sent.error());
        }
        return common::Result<api::RequestId, api::VehicleError>::Success(request.request_id);
    }

    return common::Result<api::RequestId, api::VehicleError>::Success(request.request_id);
}

common::Result<void, api::VehicleError> VehicleHalAdapter::Subscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    float sample_rate_hz,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    VehicleSubscriptionRegistry previous;
    SubscriptionChange change;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_ || !connected_) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "cannot subscribe while transport is disconnected", 0U});
        }
        previous = subscriptions_;
        const auto updated = subscriptions_.AddOrUpdate(subscriber_id, key, sample_rate_hz);
        if (!updated) {
            return common::Result<void, api::VehicleError>::Failure(updated.error());
        }
        change = updated.value();
    }

    const auto sent = SendSubscriptionUpdate(key, change, timeout);
    if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_ = std::move(previous);
    }
    return sent;
}

common::Result<void, api::VehicleError> VehicleHalAdapter::Unsubscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    VehicleSubscriptionRegistry previous;
    SubscriptionChange change;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<void, api::VehicleError>::Success();
        }
        previous = subscriptions_;
        change = subscriptions_.Remove(subscriber_id, key);
        connected = connected_;
    }

    // After transport death the desired registry is still authoritative. Removing a closed
    // client's desired subscription must succeed locally so reconnect cannot replay it.
    if (!connected) {
        return common::Result<void, api::VehicleError>::Success();
    }

    const auto sent = SendSubscriptionUpdate(key, change, timeout);
    if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_ = std::move(previous);
    }
    return sent;
}

common::Result<void, api::VehicleError> VehicleHalAdapter::ReleaseSubscriber(
    api::SubscriberId subscriber_id,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    if (timeout <= std::chrono::milliseconds::zero()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "subscriber teardown requires a positive total deadline",
             0U});
    }
    std::vector<SubscriptionUpdate> updates;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<void, api::VehicleError>::Success();
        }
        updates = subscriptions_.RemoveSubscriber(subscriber_id);
        connected = connected_;
    }
    if (!connected) {
        return common::Result<void, api::VehicleError>::Success();
    }

    std::optional<api::VehicleError> first_error;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (const auto& update : updates) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            first_error = api::VehicleError{
                api::VehicleErrorCode::kTimeout,
                "subscriber teardown exhausted its total acknowledgement budget",
                0U};
            break;
        }
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining <= std::chrono::milliseconds::zero()) {
            remaining = std::chrono::milliseconds{1};
        }
        const auto sent = SendSubscriptionUpdate(update.key, update.change, remaining);
        if (!sent) {
            first_error = sent.error();
            break;
        }
    }
    if (first_error.has_value()) {
        // Session teardown is authoritative.  A rejected, timed-out, or indeterminate unsubscribe
        // can leave the old subscription installed at the peer, so keep the desired registry
        // removed and force a session reset before any later reconnect/replay.
        const auto disconnect_error = api::VehicleError{
            api::VehicleErrorCode::kTransportDown,
            "subscriber teardown was not acknowledged; vehicle session reset is required",
            first_error->request_id};
        transport_->Shutdown();
        OnTransportDeath(disconnect_error);
        return common::Result<void, api::VehicleError>::Failure(std::move(*first_error));
    }
    return common::Result<void, api::VehicleError>::Success();
}

void VehicleHalAdapter::CancelOwner(
    api::SessionId owner_id,
    api::VehicleError error) noexcept {
    if (owner_id == 0U) {
        return;
    }
    std::vector<std::pair<api::RequestId, PendingRequest>> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->second.owner_id != owner_id) {
                ++pending;
                continue;
            }
            cancelled.emplace_back(pending->first, std::move(pending->second));
            pending = pending_.erase(pending);
        }
    }
    for (auto& item : cancelled) {
        error.request_id = item.first;
        Complete(
            std::move(item.second.completion),
            item.second.inline_completion,
            RequestResult::Failure(error));
    }
}

common::Result<void, api::VehicleError> VehicleHalAdapter::SendSubscriptionUpdate(
    api::PropertyKey key,
    const SubscriptionChange& change,
    std::chrono::milliseconds timeout) {
    if (!change.transport_update_required) {
        return common::Result<void, api::VehicleError>::Success();
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "subscription update requires a positive deadline", 0U});
    }

    struct ControlWaitState final {
        std::mutex mutex;
        std::condition_variable available;
        std::optional<RequestResult> result;
    };
    const auto wait_state = std::make_shared<ControlWaitState>();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = change.transport_unsubscribe_required
                            ? api::TransportOperation::kUnsubscribe
                            : api::TransportOperation::kSubscribe;
    request.key = key;
    request.sample_rate_hz = change.effective_rate_hz;
    const auto submitted = Submit(
        request,
        timeout,
        [wait_state](RequestResult result) {
            {
                std::lock_guard<std::mutex> lock(wait_state->mutex);
                if (!wait_state->result.has_value()) {
                    wait_state->result.emplace(std::move(result));
                }
            }
            wait_state->available.notify_all();
        },
        0U,
        true);
    if (!submitted) {
        return common::Result<void, api::VehicleError>::Failure(submitted.error());
    }

    std::unique_lock<std::mutex> wait_lock(wait_state->mutex);
    if (!wait_state->available.wait_until(
            wait_lock,
            deadline,
            [wait_state] { return wait_state->result.has_value(); })) {
        wait_lock.unlock();
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pending = pending_.find(request.request_id);
            if (pending != pending_.end()) {
                pending_.erase(pending);
                removed = true;
            }
        }
        if (!removed) {
            // A response, death, or PollTimeouts path already removed this request and therefore
            // owns its single inline completion.  Do not use a second arbitrary timeout here:
            // returning before that owner publishes would let a successful peer ACK be reported
            // as TIMEOUT and roll the desired registry back while the peer kept the new state.
            wait_lock.lock();
            wait_state->available.wait(
                wait_lock,
                [wait_state] { return wait_state->result.has_value(); });
            wait_lock.unlock();
        }
        wait_lock.lock();
        if (!wait_state->result.has_value()) {
            const auto timeout_error = api::VehicleError{
                api::VehicleErrorCode::kTimeout,
                "vehicle subscription control acknowledgement timed out",
                request.request_id};
            wait_lock.unlock();
            // The request left this process but its result is unknown.  Rolling the desired
            // registry back on the same transport epoch could diverge from a peer that applied the
            // operation and lost only its ACK, so invalidate the epoch before returning.
            transport_->Shutdown();
            OnTransportDeath(
                {api::VehicleErrorCode::kTransportDown,
                 "subscription control outcome is unknown; reconnect is required",
                 request.request_id});
            return common::Result<void, api::VehicleError>::Failure(timeout_error);
        }
    }

    auto result = std::move(*wait_state->result);
    if (!result) {
        return common::Result<void, api::VehicleError>::Failure(result.error());
    }
    return common::Result<void, api::VehicleError>::Success();
}

void VehicleHalAdapter::SetEventCallback(PropertyEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

void VehicleHalAdapter::SetTransportStateCallback(TransportStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callback_ = std::move(callback);
}

void VehicleHalAdapter::PollTimeouts() {
    std::vector<std::pair<PendingRequest, api::VehicleError>> expired;
    const auto now = clock_.Now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->second.deadline > now) {
                ++pending;
                continue;
            }
            expired.emplace_back(
                std::move(pending->second),
                api::VehicleError{api::VehicleErrorCode::kTimeout,
                                  "vehicle property request timed out", pending->first});
            pending = pending_.erase(pending);
        }
    }
    for (auto& item : expired) {
        Complete(
            std::move(item.first.completion),
            item.first.inline_completion,
            RequestResult::Failure(std::move(item.second)));
    }
}

common::Result<api::ApiVersion, api::VehicleError> VehicleHalAdapter::Reconnect() {
    if (IsExecutingCallback(callback_epoch_state_)) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInternal,
             "reconnect cannot run reentrantly from an adapter callback", 0U});
    }
    std::lock_guard<std::mutex> attempt_lock(connect_attempt_mutex_);
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    api::ApiVersion requested;
    std::vector<EffectiveSubscription> subscriptions;
    std::uint64_t attempt_token = 0U;
    {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load() &&
                   !transition_publisher_active_ && transition_controls_.empty() &&
                   !connect_attempt_active_;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "HAL adapter is shut down", 0U});
        }
        requested = requested_version_;
        subscriptions = subscriptions_.Snapshot();
        connect_attempt_active_ = true;
        connect_attempt_invalidated_ = false;
        attempt_token = ++connect_attempt_token_;
    }

    const auto connected_version = transport_->Connect(requested);
    if (!connected_version) {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load();
        });
        connect_attempt_active_ = false;
        transition_changed_.notify_all();
        return connected_version;
    }
    const bool transport_connected = transport_->IsConnected();
    std::uint64_t connection_generation = 0U;
    bool committed = false;
    bool publish_controls = false;
    {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load();
        });
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
        if (!shutdown_ && connect_attempt_active_ &&
            connect_attempt_token_ == attempt_token && !connect_attempt_invalidated_ &&
            transport_connected) {
            const auto obsolete_generation = callback_epoch_state_->epoch;
            connected_ = true;
            ++callback_epoch_state_->epoch;
            callback_epoch_state_->connected = true;
            connection_generation = callback_epoch_state_->epoch;
            publish_controls = QueueTransitionControlLocked(
                {connection_generation,
                 obsolete_generation,
                 {},
                 {},
                 true,
                 false,
                 true});
            committed = true;
        } else {
            connect_attempt_active_ = false;
            transition_changed_.notify_all();
        }
    }
    if (publish_controls) {
        DrainTransitionControls();
    }
    if (!committed) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kTransportDown,
             "vehicle transport died before reconnect commit", 0U});
    }

    for (const auto& subscription : subscriptions) {
        const auto replay = SendSubscriptionUpdate(
            subscription.key,
            {true, false, subscription.sample_rate_hz},
            std::chrono::milliseconds{1000});
        if (!replay) {
            transport_->Shutdown();
            OnTransportDeath(
                {api::VehicleErrorCode::kTransportDown,
                 "subscription replay failed; reconnect must restart the full replay",
                 replay.error().request_id});
            {
                std::lock_guard<std::mutex> transition_lock(transition_mutex_);
                if (connect_attempt_token_ == attempt_token) {
                    connect_attempt_active_ = false;
                }
                transition_changed_.notify_all();
            }
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(replay.error());
        }
    }

    if (!transport_->IsConnected()) {
        OnTransportDeath(
            {api::VehicleErrorCode::kTransportDown,
             "vehicle transport died before reconnect replay committed", 0U});
    }

    bool reconnect_committed = false;
    bool publish_connected = false;
    bool cancelled = false;
    {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        transition_changed_.wait(transition_lock, [this] {
            return handled_death_sequence_ == death_sequence_.load();
        });
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
        cancelled = shutdown_;
        if (!shutdown_ && connect_attempt_active_ &&
            connect_attempt_token_ == attempt_token && !connect_attempt_invalidated_ &&
            connected_ && callback_epoch_state_->connected &&
            callback_epoch_state_->epoch == connection_generation) {
            publish_connected = QueueTransitionControlLocked(
                {connection_generation,
                 0U,
                 state_callback_,
                 {api::VehicleErrorCode::kOk, {}, 0U},
                 false,
                 true,
                 true});
            reconnect_committed = true;
        }
        if (connect_attempt_token_ == attempt_token) {
            connect_attempt_active_ = false;
        }
        transition_changed_.notify_all();
    }
    if (publish_connected) {
        DrainTransitionControls();
    }
    if (!reconnect_committed) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {cancelled ? api::VehicleErrorCode::kCancelled
                       : api::VehicleErrorCode::kTransportDown,
             cancelled ? "HAL adapter was shut down during reconnect"
                       : "vehicle transport died during reconnect replay",
             0U});
    }
    return connected_version;
}

bool VehicleHalAdapter::IsConnected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && transport_->IsConnected();
}

bool VehicleHalAdapter::QueueTransitionControlLocked(TransitionControl control) {
    transition_controls_.push_back(std::move(control));
    if (transition_publisher_active_) {
        return false;
    }
    transition_publisher_active_ = true;
    return true;
}

void VehicleHalAdapter::DrainTransitionControls() noexcept {
    for (;;) {
        TransitionControl control;
        try {
            {
                std::lock_guard<std::mutex> transition_lock(transition_mutex_);
                if (transition_controls_.empty()) {
                    transition_publisher_active_ = false;
                    transition_changed_.notify_all();
                    return;
                }
                control = std::move(transition_controls_.front());
                transition_controls_.pop_front();
            }

            const auto epoch_state = callback_epoch_state_;
            const bool discard_obsolete_events = control.discard_obsolete_events;
            const auto obsolete_event_generation = control.obsolete_event_generation;
            auto notification =
                [epoch_state, control = std::move(control)]() mutable {
                    if (!control.notify_state || !control.callback) {
                        return;
                    }
                    if (!BeginStateCallback(
                            epoch_state,
                            control.expected_generation,
                            control.expected_connected)) {
                        epoch_state->dropped_stale_state_callbacks.fetch_add(1U);
                        return;
                    }
                    try {
                        control.callback(
                            control.expected_connected, std::move(control.error));
                    } catch (...) {
                        EndStateCallback(epoch_state);
                        throw;
                    }
                    EndStateCallback(epoch_state);
            };
            bool posted = false;
            try {
                if (discard_obsolete_events) {
                    posted = callback_executor_.PostConnectionControl(
                        obsolete_event_generation, std::move(notification));
                } else {
                    posted = callback_executor_.PostControl(std::move(notification), false);
                }
            } catch (...) {
                posted = false;
            }
            if (!posted) {
                dropped_state_callbacks_.fetch_add(1U);
            }
        } catch (...) {
            dropped_state_callbacks_.fetch_add(1U);
        }
    }
}

std::size_t VehicleHalAdapter::DroppedEventCallbackCount() const noexcept {
    return dropped_event_callbacks_.load();
}

std::size_t VehicleHalAdapter::DroppedStaleEpochEventCount() const noexcept {
    return callback_epoch_state_->dropped_stale_epoch_events.load();
}

std::size_t VehicleHalAdapter::DroppedStateCallbackCount() const noexcept {
    return dropped_state_callbacks_.load() +
           callback_epoch_state_->dropped_stale_state_callbacks.load();
}

void VehicleHalAdapter::OnResponse(api::TransportResponse response) {
    PendingRequest pending;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto item = pending_.find(response.request_id);
        if (item != pending_.end()) {
            pending = std::move(item->second);
            pending_.erase(item);
            found = true;
        }
    }
    if (!found) {
        return;
    }

    std::optional<api::VehicleError> protocol_error;
    if (response.error.request_id != response.request_id) {
        protocol_error = api::VehicleError{
            api::VehicleErrorCode::kInternal,
            "vehicle response error id does not match its envelope id",
            response.request_id};
    } else if (response.error.code != api::VehicleErrorCode::kOk &&
               response.value.has_value()) {
        protocol_error = api::VehicleError{
            api::VehicleErrorCode::kInternal,
            "failed vehicle response must not carry a property value",
            response.request_id};
    } else if (response.error.code == api::VehicleErrorCode::kOk &&
               pending.operation == api::TransportOperation::kGet &&
               (!response.value.has_value() || response.value->key != pending.expected_key)) {
        protocol_error = api::VehicleError{
            api::VehicleErrorCode::kInternal,
            "get response value does not match the requested property key",
            response.request_id};
    } else if (response.error.code == api::VehicleErrorCode::kOk &&
               pending.operation != api::TransportOperation::kGet &&
               response.value.has_value()) {
        protocol_error = api::VehicleError{
            api::VehicleErrorCode::kInternal,
            "non-get response must not carry a property value",
            response.request_id};
    }
    if (protocol_error.has_value()) {
        Complete(
            std::move(pending.completion),
            pending.inline_completion,
            RequestResult::Failure(*protocol_error));
        transport_->Shutdown();
        OnTransportDeath(std::move(*protocol_error));
        return;
    }

    if (response.error.code != api::VehicleErrorCode::kOk) {
        response.error.request_id = response.request_id;
        Complete(
            std::move(pending.completion),
            pending.inline_completion,
            RequestResult::Failure(std::move(response.error)));
        return;
    }
    Complete(
        std::move(pending.completion),
        pending.inline_completion,
        RequestResult::Success(std::move(response.value)));
}

void VehicleHalAdapter::OnEvent(api::PropertyEvent event) {
    PropertyEventCallback callback;
    std::uint64_t event_epoch = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_ || !connected_) {
            return;
        }
        callback = event_callback_;
        std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
        if (!callback_epoch_state_->alive || !callback_epoch_state_->connected) {
            return;
        }
        event_epoch = callback_epoch_state_->epoch;
    }
    if (callback) {
        bool posted = false;
        try {
            const auto key = EventCoalescingKey(event.value.key, event_epoch);
            const auto epoch_state = callback_epoch_state_;
            posted = callback_executor_.PostEventCoalescing(
                key,
                event_epoch,
                [epoch_state,
                 event_epoch,
                 callback = std::move(callback),
                 event = std::move(event)]() mutable {
                    if (!BeginEventCallback(epoch_state, event_epoch)) {
                        epoch_state->dropped_stale_epoch_events.fetch_add(1U);
                        return;
                    }
                    try {
                        callback(std::move(event));
                    } catch (...) {
                        EndEventCallback(epoch_state);
                        throw;
                    }
                    EndEventCallback(epoch_state);
                });
        } catch (...) {
            posted = false;
        }
        if (!posted) {
            dropped_event_callbacks_.fetch_add(1U);
        }
    }
}

void VehicleHalAdapter::OnTransportDeath(api::VehicleError error) {
    const auto death_sequence = death_sequence_.fetch_add(1U) + 1U;
    std::vector<std::pair<api::RequestId, PendingRequest>> completions;
    bool publish_controls = false;
    {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        if (death_sequence <= handled_death_sequence_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            handled_death_sequence_ = death_sequence;
            transition_changed_.notify_all();
            return;
        }
        if (!connected_ &&
            (!connect_attempt_active_ || connect_attempt_invalidated_)) {
            handled_death_sequence_ = death_sequence;
            transition_changed_.notify_all();
            return;
        }
        if (connect_attempt_active_) {
            connect_attempt_invalidated_ = true;
        }
        connected_ = false;
        {
            std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
            const auto obsolete_generation = callback_epoch_state_->epoch;
            ++callback_epoch_state_->epoch;
            callback_epoch_state_->connected = false;
            publish_controls = QueueTransitionControlLocked(
                {callback_epoch_state_->epoch,
                 obsolete_generation,
                 state_callback_,
                 error,
                 true,
                 true,
                 false});
        }
        for (auto& item : pending_) {
            completions.emplace_back(item.first, std::move(item.second));
        }
        pending_.clear();
        handled_death_sequence_ = death_sequence;
        transition_changed_.notify_all();
    }

    if (!IsExecutingCallback(callback_epoch_state_)) {
        std::unique_lock<std::mutex> epoch_lock(callback_epoch_state_->mutex);
        callback_epoch_state_->idle.wait(
            epoch_lock,
            [this] { return callback_epoch_state_->active_event_callbacks == 0U; });
    }

    if (publish_controls) {
        DrainTransitionControls();
    }

    for (auto& completion : completions) {
        auto request_error = error;
        request_error.request_id = completion.first;
        Complete(
            std::move(completion.second.completion),
            completion.second.inline_completion,
            RequestResult::Failure(std::move(request_error)));
    }
}

bool VehicleHalAdapter::BeginEventCallback(
    const std::shared_ptr<CallbackEpochState>& state,
    std::uint64_t epoch) noexcept {
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->alive || !state->connected || state->epoch != epoch) {
            return false;
        }
        ++state->active_event_callbacks;
        ++state->active_callbacks;
        try {
            g_callback_state_stack.push_back(state.get());
        } catch (...) {
            --state->active_event_callbacks;
            --state->active_callbacks;
            state->idle.notify_all();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void VehicleHalAdapter::EndEventCallback(
    const std::shared_ptr<CallbackEpochState>& state) noexcept {
    try {
        const auto active = std::find(
            g_callback_state_stack.rbegin(), g_callback_state_stack.rend(), state.get());
        if (active != g_callback_state_stack.rend()) {
            g_callback_state_stack.erase(std::next(active).base());
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_event_callbacks > 0U) {
            --state->active_event_callbacks;
        }
        if (state->active_callbacks > 0U) {
            --state->active_callbacks;
        }
        state->idle.notify_all();
    } catch (...) {
    }
}

bool VehicleHalAdapter::BeginStateCallback(
    const std::shared_ptr<CallbackEpochState>& state,
    std::uint64_t generation,
    bool expected_connected) noexcept {
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->alive || state->epoch != generation ||
            state->connected != expected_connected) {
            return false;
        }
        ++state->active_callbacks;
        try {
            g_callback_state_stack.push_back(state.get());
        } catch (...) {
            --state->active_callbacks;
            state->idle.notify_all();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void VehicleHalAdapter::EndStateCallback(
    const std::shared_ptr<CallbackEpochState>& state) noexcept {
    try {
        const auto active = std::find(
            g_callback_state_stack.rbegin(), g_callback_state_stack.rend(), state.get());
        if (active != g_callback_state_stack.rend()) {
            g_callback_state_stack.erase(std::next(active).base());
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_callbacks > 0U) {
            --state->active_callbacks;
        }
        state->idle.notify_all();
    } catch (...) {
    }
}

bool VehicleHalAdapter::IsExecutingCallback(
    const std::shared_ptr<CallbackEpochState>& state) noexcept {
    return std::find(g_callback_state_stack.begin(), g_callback_state_stack.end(), state.get()) !=
           g_callback_state_stack.end();
}

void VehicleHalAdapter::Complete(
    RequestCompletion completion,
    bool inline_completion,
    RequestResult result) {
    if (inline_completion) {
        completion(std::move(result));
        return;
    }
    auto notification =
        [completion = std::move(completion), result = std::move(result)]() mutable {
            completion(std::move(result));
        };
    bool posted = false;
    try {
        posted = callback_executor_.Post(notification);
    } catch (...) {
        posted = false;
    }
    if (!posted) {
        notification();
    }
}

api::RequestId VehicleHalAdapter::NextRequestId() noexcept {
    api::RequestId candidate = next_request_id_.fetch_add(1U);
    if (candidate == 0U) {
        candidate = next_request_id_.fetch_add(1U);
    }
    return candidate;
}

void VehicleHalAdapter::Shutdown() noexcept {
    std::lock_guard<std::mutex> attempt_lock(connect_attempt_mutex_);
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    std::vector<std::pair<api::RequestId, PendingRequest>> completions;
    {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        connected_ = false;
        connect_attempt_invalidated_ = true;
        connect_attempt_active_ = false;
        transition_controls_.clear();
        transition_changed_.notify_all();
        {
            std::lock_guard<std::mutex> epoch_lock(callback_epoch_state_->mutex);
            callback_epoch_state_->alive = false;
            callback_epoch_state_->connected = false;
            ++callback_epoch_state_->epoch;
        }
        event_callback_ = {};
        state_callback_ = {};
        subscriptions_.Clear();
        for (auto& item : pending_) {
            completions.emplace_back(item.first, std::move(item.second));
        }
        pending_.clear();
    }
    transport_->SetCallbacks({});
    transport_->Shutdown();
    if (!IsExecutingCallback(callback_epoch_state_)) {
        {
            std::unique_lock<std::mutex> transition_lock(transition_mutex_);
            transition_changed_.wait(
                transition_lock,
                [this] { return !transition_publisher_active_; });
        }
        std::unique_lock<std::mutex> epoch_lock(callback_epoch_state_->mutex);
        callback_epoch_state_->idle.wait(
            epoch_lock,
            [this] { return callback_epoch_state_->active_callbacks == 0U; });
    }
    for (auto& completion : completions) {
        Complete(
            std::move(completion.second.completion),
            completion.second.inline_completion,
            RequestResult::Failure(
                {api::VehicleErrorCode::kCancelled,
                 "HAL adapter shut down",
                 completion.first}));
    }
}

}  // namespace fw03::hal
