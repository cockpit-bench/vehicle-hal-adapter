#include "fw03/middleware/vehicle_property_gateway.h"

#include <utility>

namespace fw03::middleware {

VehiclePropertyGateway::VehiclePropertyGateway(hal::VehicleHalAdapter& hal_adapter)
    : hal_adapter_(hal_adapter) {
    hal_adapter_.SetEventCallback(
        [this](api::PropertyEvent event) { OnEvent(std::move(event)); });
    hal_adapter_.SetTransportStateCallback(
        [this](bool connected, api::VehicleError error) {
            OnTransportState(connected, std::move(error));
        });
}

VehiclePropertyGateway::~VehiclePropertyGateway() { Shutdown(); }

common::Result<api::ApiVersion, api::VehicleError> VehiclePropertyGateway::Start(
    const api::ApiVersion& requested_version) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "property gateway is shut down", 0U});
        }
    }
    return hal_adapter_.Start(requested_version);
}

common::Result<api::RequestId, api::VehicleError> VehiclePropertyGateway::Get(
    api::PropertyKey key,
    std::chrono::milliseconds timeout,
    ValueCompletion completion,
    bool prefer_cache) {
    if (!completion) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "get completion is required", 0U});
    }
    if (prefer_cache) {
        std::optional<api::VehiclePropertyValue> cached;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto item = cache_.find(key);
            if (item != cache_.end() &&
                item->second.value.status == api::PropertyStatus::kAvailable) {
                cached = item->second.value;
            }
        }
        if (cached.has_value()) {
            completion(ValueResult::Success(std::move(cached)));
            return common::Result<api::RequestId, api::VehicleError>::Success(0U);
        }
    }
    return hal_adapter_.Get(key, timeout, std::move(completion));
}

common::Result<api::RequestId, api::VehicleError> VehiclePropertyGateway::Set(
    api::VehiclePropertyValue value,
    std::chrono::milliseconds timeout,
    ValueCompletion completion) {
    return hal_adapter_.Set(std::move(value), timeout, std::move(completion));
}

common::Result<void, api::VehicleError> VehiclePropertyGateway::Subscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    float sample_rate_hz,
    ClientEventCallback callback) {
    if (!callback) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "subscription callback is required", 0U});
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "property gateway is shut down", 0U});
        }
        callbacks_[subscriber_id][key] = std::move(callback);
    }

    const auto subscribed = hal_adapter_.Subscribe(subscriber_id, key, sample_rate_hz);
    if (!subscribed) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto subscriber = callbacks_.find(subscriber_id);
        if (subscriber != callbacks_.end()) {
            subscriber->second.erase(key);
            if (subscriber->second.empty()) {
                callbacks_.erase(subscriber);
            }
        }
    }
    return subscribed;
}

common::Result<void, api::VehicleError> VehiclePropertyGateway::Unsubscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key) {
    const auto unsubscribed = hal_adapter_.Unsubscribe(subscriber_id, key);
    if (!unsubscribed) {
        return unsubscribed;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto subscriber = callbacks_.find(subscriber_id);
    if (subscriber != callbacks_.end()) {
        subscriber->second.erase(key);
        if (subscriber->second.empty()) {
            callbacks_.erase(subscriber);
        }
    }
    return common::Result<void, api::VehicleError>::Success();
}

void VehiclePropertyGateway::SetStateCallback(GatewayStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callback_ = std::move(callback);
}

void VehiclePropertyGateway::PollTimeouts() { hal_adapter_.PollTimeouts(); }

common::Result<api::ApiVersion, api::VehicleError> VehiclePropertyGateway::Reconnect() {
    const auto reconnected = hal_adapter_.Reconnect();
    if (reconnected) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }
    return reconnected;
}

bool VehiclePropertyGateway::IsConnected() const noexcept { return hal_adapter_.IsConnected(); }

void VehiclePropertyGateway::OnEvent(api::PropertyEvent event) {
    std::vector<ClientEventCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        const auto cached = cache_.find(event.value.key);
        if (cached != cache_.end() &&
            (event.sequence <= cached->second.sequence || event.value == cached->second.value)) {
            return;
        }
        cache_[event.value.key] = {event.sequence, event.value};
        for (const auto& subscriber : callbacks_) {
            const auto callback = subscriber.second.find(event.value.key);
            if (callback != subscriber.second.end()) {
                callbacks.push_back(callback->second);
            }
        }
    }
    for (auto& callback : callbacks) {
        callback(event);
    }
}

void VehiclePropertyGateway::OnTransportState(bool connected, api::VehicleError error) {
    GatewayStateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        if (!connected) {
            cache_.clear();
        }
        callback = state_callback_;
    }
    if (callback) {
        callback(connected, std::move(error));
    }
}

void VehiclePropertyGateway::Shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        callbacks_.clear();
        state_callback_ = {};
    }
    hal_adapter_.SetEventCallback({});
    hal_adapter_.SetTransportStateCallback({});
    hal_adapter_.Shutdown();
}

}  // namespace fw03::middleware
