#include "fw03/application/vehicle_service.h"

#include <utility>

namespace fw03::application {

VehicleService::VehicleService(middleware::VehiclePropertyGateway& gateway) : gateway_(gateway) {
    gateway_.SetStateCallback(
        [this](bool connected, api::VehicleError error) {
            DispatchTransportState(connected, std::move(error));
        });
}

VehicleService::~VehicleService() { Shutdown(); }

common::Result<api::ApiVersion, api::VehicleError> VehicleService::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "vehicle service is shut down", 0U});
        }
        if (started_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Success(
                api::CurrentApiVersion());
        }
    }
    const auto started = gateway_.Start(api::CurrentApiVersion());
    if (started) {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
    }
    return started;
}

common::Result<api::SessionId, api::VehicleError> VehicleService::OpenSession(
    CallerContext caller,
    api::ApiVersion requested_version,
    SessionCallbacks callbacks) {
    if (caller.client_name.empty() || !callbacks.on_property_event) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "session requires a client name and property-event callback", 0U});
    }
    const auto negotiated = api::NegotiateApiVersion(requested_version, api::CurrentApiVersion());
    if (!negotiated) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(negotiated.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_ || !started_) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kTransportDown, "vehicle service has not started", 0U});
    }
    const api::SessionId session_id = next_session_id_++;
    sessions_.emplace(
        session_id,
        Session{std::move(caller), negotiated.value(), std::move(callbacks), {}});
    return common::Result<api::SessionId, api::VehicleError>::Success(session_id);
}

common::Result<void, api::VehicleError> VehicleService::CloseSession(api::SessionId session_id) {
    std::set<api::PropertyKey> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = sessions_.find(session_id);
        if (session == sessions_.end()) {
            return common::Result<void, api::VehicleError>::Success();
        }
        subscriptions = std::move(session->second.subscriptions);
        sessions_.erase(session);
    }

    std::optional<api::VehicleError> first_error;
    for (const auto& key : subscriptions) {
        const auto result = gateway_.Unsubscribe(session_id, key);
        if (!result && !first_error.has_value()) {
            first_error = result.error();
        }
    }
    if (first_error.has_value()) {
        return common::Result<void, api::VehicleError>::Failure(std::move(*first_error));
    }
    return common::Result<void, api::VehicleError>::Success();
}

common::Result<api::RequestId, api::VehicleError> VehicleService::Get(
    api::SessionId session_id,
    api::PropertyKey key,
    std::chrono::milliseconds timeout,
    middleware::ValueCompletion completion,
    bool prefer_cache) {
    const auto access = CheckAccess(session_id, key.property_id, false);
    if (!access) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(access.error());
    }
    return gateway_.Get(key, timeout, std::move(completion), prefer_cache);
}

common::Result<api::RequestId, api::VehicleError> VehicleService::Set(
    api::SessionId session_id,
    api::VehiclePropertyValue value,
    std::chrono::milliseconds timeout,
    middleware::ValueCompletion completion) {
    const auto access = CheckAccess(session_id, value.key.property_id, true);
    if (!access) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(access.error());
    }
    return gateway_.Set(std::move(value), timeout, std::move(completion));
}

common::Result<void, api::VehicleError> VehicleService::Subscribe(
    api::SessionId session_id,
    api::PropertyKey key,
    float sample_rate_hz) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto session = sessions_.find(session_id);
    if (session == sessions_.end()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "unknown client session", 0U});
    }
    if (session->second.caller.readable_properties.find(key.property_id) ==
        session->second.caller.readable_properties.end()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "caller may not subscribe to this property", 0U});
    }
    const auto subscribed = gateway_.Subscribe(
        session_id,
        key,
        sample_rate_hz,
        [this, session_id](api::PropertyEvent event) {
            DispatchEvent(session_id, std::move(event));
        });
    if (subscribed) {
        session->second.subscriptions.insert(key);
    }
    return subscribed;
}

common::Result<void, api::VehicleError> VehicleService::Unsubscribe(
    api::SessionId session_id,
    api::PropertyKey key) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto session = sessions_.find(session_id);
    if (session == sessions_.end()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "unknown client session", 0U});
    }
    const auto unsubscribed = gateway_.Unsubscribe(session_id, key);
    if (unsubscribed) {
        session->second.subscriptions.erase(key);
    }
    return unsubscribed;
}

common::Result<void, api::VehicleError> VehicleService::CheckAccess(
    api::SessionId session_id,
    std::uint32_t property_id,
    bool write) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(session_id);
    if (session == sessions_.end()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "unknown client session", 0U});
    }
    const auto& allowed = write ? session->second.caller.writable_properties
                                : session->second.caller.readable_properties;
    if (allowed.find(property_id) == allowed.end()) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             write ? "caller may not write this property" : "caller may not read this property",
             0U});
    }
    return common::Result<void, api::VehicleError>::Success();
}

void VehicleService::DispatchEvent(api::SessionId session_id, api::PropertyEvent event) {
    std::function<void(api::PropertyEvent)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = sessions_.find(session_id);
        if (session == sessions_.end() ||
            session->second.subscriptions.find(event.value.key) ==
                session->second.subscriptions.end()) {
            return;
        }
        callback = session->second.callbacks.on_property_event;
    }
    callback(std::move(event));
}

void VehicleService::DispatchTransportState(bool connected, api::VehicleError error) {
    std::vector<std::function<void(bool, api::VehicleError)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        for (const auto& session : sessions_) {
            if (session.second.callbacks.on_transport_state) {
                callbacks.push_back(session.second.callbacks.on_transport_state);
            }
        }
    }
    for (auto& callback : callbacks) {
        callback(connected, error);
    }
}

void VehicleService::PollTimeouts() { gateway_.PollTimeouts(); }

common::Result<api::ApiVersion, api::VehicleError> VehicleService::Reconnect() {
    return gateway_.Reconnect();
}

bool VehicleService::IsConnected() const noexcept { return gateway_.IsConnected(); }

void VehicleService::Shutdown() noexcept {
    std::map<api::SessionId, Session> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        started_ = false;
        sessions = std::move(sessions_);
    }
    for (const auto& session : sessions) {
        for (const auto& key : session.second.subscriptions) {
            const auto ignored = gateway_.Unsubscribe(session.first, key);
            (void)ignored;
        }
    }
    gateway_.SetStateCallback({});
    gateway_.Shutdown();
}

}  // namespace fw03::application
