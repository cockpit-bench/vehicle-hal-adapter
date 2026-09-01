#include "fw03/hal/vehicle_hal_adapter.h"

#include <utility>

namespace fw03::hal {

VehicleHalAdapter::VehicleHalAdapter(
    std::shared_ptr<platform::VehicleTransport> transport,
    common::Clock& clock,
    common::TaskExecutor& callback_executor)
    : transport_(std::move(transport)), clock_(clock), callback_executor_(callback_executor) {
    transport_->SetCallbacks({
        [this](api::TransportResponse response) { OnResponse(std::move(response)); },
        [this](api::PropertyEvent event) { OnEvent(std::move(event)); },
        [this](api::VehicleError error) { OnTransportDeath(std::move(error)); },
    });
}

VehicleHalAdapter::~VehicleHalAdapter() { Shutdown(); }

common::Result<api::ApiVersion, api::VehicleError> VehicleHalAdapter::Start(
    const api::ApiVersion& requested_version) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "HAL adapter is shut down", 0U});
        }
        requested_version_ = requested_version;
    }

    const auto connected_version = transport_->Connect(requested_version);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = connected_version.ok();
    }
    return connected_version;
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Get(
    api::PropertyKey key,
    std::chrono::milliseconds timeout,
    RequestCompletion completion) {
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = api::TransportOperation::kGet;
    request.key = key;
    return Submit(std::move(request), timeout, std::move(completion));
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Set(
    api::VehiclePropertyValue value,
    std::chrono::milliseconds timeout,
    RequestCompletion completion) {
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = api::TransportOperation::kSet;
    request.key = value.key;
    request.value = std::move(value);
    return Submit(std::move(request), timeout, std::move(completion));
}

common::Result<api::RequestId, api::VehicleError> VehicleHalAdapter::Submit(
    api::TransportRequest request,
    std::chrono::milliseconds timeout,
    RequestCompletion completion) {
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
        pending_.emplace(
            request.request_id,
            PendingRequest{request.operation, clock_.Now() + timeout, std::move(completion)});
    }

    const auto sent = transport_->Send(request);
    if (!sent) {
        RequestCompletion failed_completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pending = pending_.find(request.request_id);
            if (pending != pending_.end()) {
                failed_completion = std::move(pending->second.completion);
                pending_.erase(pending);
            }
        }
        if (failed_completion) {
            Complete(
                std::move(failed_completion),
                RequestResult::Failure(sent.error()));
        }
        return common::Result<api::RequestId, api::VehicleError>::Failure(sent.error());
    }

    return common::Result<api::RequestId, api::VehicleError>::Success(request.request_id);
}

common::Result<void, api::VehicleError> VehicleHalAdapter::Subscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    float sample_rate_hz) {
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

    const auto sent = SendSubscriptionUpdate(key, change);
    if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_ = std::move(previous);
    }
    return sent;
}

common::Result<void, api::VehicleError> VehicleHalAdapter::Unsubscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key) {
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

    const auto sent = SendSubscriptionUpdate(key, change);
    if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_ = std::move(previous);
    }
    return sent;
}

common::Result<void, api::VehicleError> VehicleHalAdapter::SendSubscriptionUpdate(
    api::PropertyKey key,
    const SubscriptionChange& change) {
    if (!change.transport_update_required) {
        return common::Result<void, api::VehicleError>::Success();
    }
    api::TransportRequest request;
    request.request_id = NextRequestId();
    request.operation = change.transport_unsubscribe_required
                            ? api::TransportOperation::kUnsubscribe
                            : api::TransportOperation::kSubscribe;
    request.key = key;
    request.sample_rate_hz = change.effective_rate_hz;
    return transport_->Send(request);
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
    std::vector<std::pair<RequestCompletion, api::VehicleError>> expired;
    const auto now = clock_.Now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->second.deadline > now) {
                ++pending;
                continue;
            }
            expired.emplace_back(
                std::move(pending->second.completion),
                api::VehicleError{api::VehicleErrorCode::kTimeout,
                                  "vehicle property request timed out", pending->first});
            pending = pending_.erase(pending);
        }
    }
    for (auto& item : expired) {
        Complete(std::move(item.first), RequestResult::Failure(std::move(item.second)));
    }
}

common::Result<api::ApiVersion, api::VehicleError> VehicleHalAdapter::Reconnect() {
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    api::ApiVersion requested;
    std::vector<EffectiveSubscription> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "HAL adapter is shut down", 0U});
        }
        requested = requested_version_;
        subscriptions = subscriptions_.Snapshot();
    }

    const auto connected_version = transport_->Connect(requested);
    if (!connected_version) {
        return connected_version;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = true;
    }

    for (const auto& subscription : subscriptions) {
        const auto replay = SendSubscriptionUpdate(
            subscription.key,
            {true, false, subscription.sample_rate_hz});
        if (!replay) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(replay.error());
        }
    }

    TransportStateCallback state_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_callback = state_callback_;
    }
    if (state_callback) {
        const auto posted = callback_executor_.Post(
            [state_callback = std::move(state_callback)]() mutable {
                state_callback(true, {api::VehicleErrorCode::kOk, {}, 0U});
            });
        (void)posted;
    }
    return connected_version;
}

bool VehicleHalAdapter::IsConnected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && transport_->IsConnected();
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

    if (response.error.code != api::VehicleErrorCode::kOk) {
        response.error.request_id = response.request_id;
        Complete(std::move(pending.completion), RequestResult::Failure(std::move(response.error)));
        return;
    }
    if (pending.operation == api::TransportOperation::kGet && !response.value.has_value()) {
        Complete(
            std::move(pending.completion),
            RequestResult::Failure({api::VehicleErrorCode::kInternal,
                                    "get response did not contain a property value",
                                    response.request_id}));
        return;
    }
    Complete(
        std::move(pending.completion),
        RequestResult::Success(std::move(response.value)));
}

void VehicleHalAdapter::OnEvent(api::PropertyEvent event) {
    PropertyEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        const auto posted = callback_executor_.Post(
            [callback = std::move(callback), event = std::move(event)]() mutable {
                callback(std::move(event));
            });
        (void)posted;
    }
}

void VehicleHalAdapter::OnTransportDeath(api::VehicleError error) {
    std::vector<RequestCompletion> completions;
    TransportStateCallback state_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        connected_ = false;
        for (auto& item : pending_) {
            completions.push_back(std::move(item.second.completion));
        }
        pending_.clear();
        state_callback = state_callback_;
    }

    for (auto& completion : completions) {
        Complete(std::move(completion), RequestResult::Failure(error));
    }
    if (state_callback) {
        const auto posted = callback_executor_.Post(
            [state_callback = std::move(state_callback), error = std::move(error)]() mutable {
                state_callback(false, std::move(error));
            });
        (void)posted;
    }
}

void VehicleHalAdapter::Complete(RequestCompletion completion, RequestResult result) {
    const auto posted = callback_executor_.Post(
        [completion = std::move(completion), result = std::move(result)]() mutable {
            completion(std::move(result));
        });
    (void)posted;
}

api::RequestId VehicleHalAdapter::NextRequestId() noexcept {
    api::RequestId candidate = next_request_id_.fetch_add(1U);
    if (candidate == 0U) {
        candidate = next_request_id_.fetch_add(1U);
    }
    return candidate;
}

void VehicleHalAdapter::Shutdown() noexcept {
    std::lock_guard<std::mutex> update_lock(subscription_update_mutex_);
    std::vector<RequestCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        connected_ = false;
        event_callback_ = {};
        state_callback_ = {};
        subscriptions_.Clear();
        for (auto& item : pending_) {
            completions.push_back(std::move(item.second.completion));
        }
        pending_.clear();
    }
    transport_->SetCallbacks({});
    transport_->Shutdown();
    for (auto& completion : completions) {
        Complete(
            std::move(completion),
            RequestResult::Failure(
                {api::VehicleErrorCode::kCancelled, "HAL adapter shut down", 0U}));
    }
}

}  // namespace fw03::hal
