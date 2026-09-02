#include "fw03/application/vehicle_client_session.h"

#include "fw03/api/vehicle_contract.h"

#include <utility>

namespace fw03::application {

PropertyAllowlistPolicy::PropertyAllowlistPolicy(
    std::set<std::uint32_t> allowed_user_ids,
    std::set<std::uint32_t> allowed_group_ids,
    std::set<api::PropertyKey> readable_properties,
    std::set<api::PropertyKey> writable_properties)
    : allowed_user_ids_(std::move(allowed_user_ids)),
      allowed_group_ids_(std::move(allowed_group_ids)),
      readable_properties_(std::move(readable_properties)),
      writable_properties_(std::move(writable_properties)) {}

common::Result<CallerContext, api::VehicleError> PropertyAllowlistPolicy::Authorize(
    const api::PeerCredentials& peer) const {
    if (peer.process_id <= 0 || allowed_user_ids_.empty() || allowed_group_ids_.empty()) {
        return common::Result<CallerContext, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "client authorization requires peer credentials and explicit uid/gid rules", 0U});
    }
    if (allowed_user_ids_.find(peer.user_id) == allowed_user_ids_.end() ||
        allowed_group_ids_.find(peer.group_id) == allowed_group_ids_.end()) {
        return common::Result<CallerContext, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "client uid or gid is not authorized for the vehicle gateway", 0U});
    }
    if (readable_properties_.empty() && writable_properties_.empty()) {
        return common::Result<CallerContext, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "the client property allowlist is empty", 0U});
    }

    CallerContext caller;
    caller.client_name =
        "uid=" + std::to_string(peer.user_id) + ",gid=" + std::to_string(peer.group_id) +
        ",pid=" + std::to_string(peer.process_id);
    caller.readable_properties = readable_properties_;
    caller.writable_properties = writable_properties_;
    return common::Result<CallerContext, api::VehicleError>::Success(std::move(caller));
}

VehicleClientSession::VehicleClientSession(
    VehicleService& service,
    const ClientAccessPolicy& access_policy,
    std::chrono::milliseconds request_timeout,
    std::size_t maximum_in_flight)
    : service_(service),
      access_policy_(access_policy),
      request_timeout_(request_timeout),
      maximum_in_flight_(maximum_in_flight),
      state_(std::make_shared<SharedState>()) {}

VehicleClientSession::~VehicleClientSession() { Close(); }

common::Result<api::ApiVersion, api::VehicleError> VehicleClientSession::Open(
    const api::PeerCredentials& peer,
    const api::ApiVersion& requested_version,
    api::ClientMessageSink outbound) {
    if (!outbound || request_timeout_ <= std::chrono::milliseconds::zero() ||
        maximum_in_flight_ == 0U) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "client session requires an outbound sink and a positive request timeout", 0U});
    }

    const auto caller = access_policy_.Authorize(peer);
    if (!caller) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(caller.error());
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (session_id_.has_value()) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "client session is already open", 0U});
    }

    const std::weak_ptr<SharedState> weak_state = state_;
    SessionCallbacks callbacks;
    callbacks.on_property_event = [weak_state](api::PropertyEvent event) {
        const auto state = weak_state.lock();
        if (state) {
            (void)Send(state, api::WireMessage{std::move(event)});
        }
    };

    const auto opened = service_.OpenSession(caller.value(), requested_version, std::move(callbacks));
    if (!opened) {
        return common::Result<api::ApiVersion, api::VehicleError>::Failure(opened.error());
    }
    {
        std::lock_guard<std::mutex> state_lock(state_->mutex);
        state_->outbound = std::move(outbound);
        state_->pending_request_ids.clear();
        state_->last_request_id = 0U;
        state_->open = true;
    }
    session_id_ = opened.value();
    const auto negotiated = api::NegotiateApiVersion(requested_version, api::CurrentApiVersion());
    if (!negotiated) {
        const auto ignored = service_.CloseSession(*session_id_);
        (void)ignored;
        session_id_.reset();
        std::lock_guard<std::mutex> state_lock(state_->mutex);
        state_->open = false;
        state_->outbound = {};
        return negotiated;
    }
    return negotiated;
}

void VehicleClientSession::HandleRequest(api::TransportRequest request) {
    const auto valid = api::ValidateRequest(request);
    if (!valid) {
        auto error = valid.error();
        error.request_id = request.request_id;
        SendImmediateResponse(
            state_,
            request.request_id,
            common::Result<void, api::VehicleError>::Failure(std::move(error)));
        return;
    }

    const bool reserve_in_flight =
        request.operation == api::TransportOperation::kGet ||
        request.operation == api::TransportOperation::kSet;
    const auto session = ValidateRequestSequence(request.request_id, reserve_in_flight);
    if (!session) {
        SendImmediateResponse(
            state_,
            request.request_id,
            common::Result<void, api::VehicleError>::Failure(session.error()));
        return;
    }

    const auto request_id = request.request_id;
    switch (request.operation) {
        case api::TransportOperation::kGet: {
            const auto submitted = service_.Get(
                session.value(),
                request.key,
                request_timeout_,
                [state = state_, request_id](middleware::ValueResult result) {
                    SendResponse(state, request_id, std::move(result));
                },
                true);
            if (!submitted) {
                auto error = submitted.error();
                error.request_id = request_id;
                SendResponse(
                    state_,
                    request_id,
                    middleware::ValueResult::Failure(std::move(error)));
            }
            return;
        }
        case api::TransportOperation::kSet: {
            const auto submitted = service_.Set(
                session.value(),
                *request.value,
                request_timeout_,
                [state = state_, request_id](middleware::ValueResult result) {
                    SendResponse(state, request_id, std::move(result));
                });
            if (!submitted) {
                auto error = submitted.error();
                error.request_id = request_id;
                SendResponse(
                    state_,
                    request_id,
                    middleware::ValueResult::Failure(std::move(error)));
            }
            return;
        }
        case api::TransportOperation::kSubscribe:
            SendImmediateResponse(
                state_,
                request_id,
                service_.Subscribe(
                    session.value(), request.key, request.sample_rate_hz, request_timeout_));
            return;
        case api::TransportOperation::kUnsubscribe:
            SendImmediateResponse(
                state_,
                request_id,
                service_.Unsubscribe(session.value(), request.key, request_timeout_));
            return;
    }

    SendImmediateResponse(
        state_,
        request_id,
        common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kNotSupported, "unsupported client operation", request_id}));
}

void VehicleClientSession::Close() noexcept {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!session_id_.has_value()) {
        return;
    }
    const auto session_id = *session_id_;
    session_id_.reset();
    {
        std::lock_guard<std::mutex> state_lock(state_->mutex);
        state_->open = false;
        state_->outbound = {};
        state_->pending_request_ids.clear();
    }
    lifecycle_lock.unlock();
    const auto ignored = service_.CloseSession(session_id);
    (void)ignored;
}

void VehicleClientSession::SendResponse(
    const std::shared_ptr<SharedState>& state,
    api::RequestId request_id,
    middleware::ValueResult result) noexcept {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pending_request_ids.erase(request_id);
    }
    api::TransportResponse response;
    response.request_id = request_id;
    if (result) {
        response.error = {api::VehicleErrorCode::kOk, {}, request_id};
        response.value = result.value();
    } else {
        response.error = result.error();
        response.error.request_id = request_id;
    }
    (void)Send(state, api::WireMessage{std::move(response)});
}

void VehicleClientSession::SendImmediateResponse(
    const std::shared_ptr<SharedState>& state,
    api::RequestId request_id,
    common::Result<void, api::VehicleError> result) noexcept {
    api::TransportResponse response;
    response.request_id = request_id;
    if (result) {
        response.error = {api::VehicleErrorCode::kOk, {}, request_id};
    } else {
        response.error = result.error();
        response.error.request_id = request_id;
    }
    (void)Send(state, api::WireMessage{std::move(response)});
}

bool VehicleClientSession::Send(
    const std::shared_ptr<SharedState>& state,
    api::WireMessage message) noexcept {
    api::ClientMessageSink outbound;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->open || !state->outbound) {
            return false;
        }
        outbound = state->outbound;
    }
    try {
        return outbound(std::move(message));
    } catch (...) {
        return false;
    }
}

common::Result<api::SessionId, api::VehicleError> VehicleClientSession::ValidateRequestSequence(
    api::RequestId request_id,
    bool reserve_in_flight) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!session_id_.has_value()) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kTransportDown, "client session is closed", request_id});
    }
    std::lock_guard<std::mutex> state_lock(state_->mutex);
    if (!state_->open || request_id == 0U || request_id <= state_->last_request_id) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "client request identifiers must be non-zero and strictly increasing", request_id});
    }
    if (reserve_in_flight && state_->pending_request_ids.size() >= maximum_in_flight_) {
        return common::Result<api::SessionId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInternal,
             "client in-flight request capacity is exhausted", request_id});
    }
    state_->last_request_id = request_id;
    if (reserve_in_flight) {
        state_->pending_request_ids.insert(request_id);
    }
    return common::Result<api::SessionId, api::VehicleError>::Success(*session_id_);
}

}  // namespace fw03::application
