#pragma once

#include "fw03/api/client_session_port.h"
#include "fw03/application/vehicle_service.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace fw03::application {

class ClientAccessPolicy {
public:
    virtual ~ClientAccessPolicy() = default;

    [[nodiscard]] virtual common::Result<CallerContext, api::VehicleError> Authorize(
        const api::PeerCredentials& peer) const = 0;
};

class PropertyAllowlistPolicy final : public ClientAccessPolicy {
public:
    PropertyAllowlistPolicy(
        std::set<std::uint32_t> allowed_user_ids,
        std::set<std::uint32_t> allowed_group_ids,
        std::set<api::PropertyKey> readable_properties,
        std::set<api::PropertyKey> writable_properties);

    [[nodiscard]] common::Result<CallerContext, api::VehicleError> Authorize(
        const api::PeerCredentials& peer) const override;

private:
    const std::set<std::uint32_t> allowed_user_ids_;
    const std::set<std::uint32_t> allowed_group_ids_;
    const std::set<api::PropertyKey> readable_properties_;
    const std::set<api::PropertyKey> writable_properties_;
};

class VehicleClientSession final : public api::ClientRequestSession {
public:
    VehicleClientSession(
        VehicleService& service,
        const ClientAccessPolicy& access_policy,
        std::chrono::milliseconds request_timeout,
        std::size_t maximum_in_flight = 32U);
    ~VehicleClientSession() override;

    VehicleClientSession(const VehicleClientSession&) = delete;
    VehicleClientSession& operator=(const VehicleClientSession&) = delete;

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Open(
        const api::PeerCredentials& peer,
        const api::ApiVersion& requested_version,
        api::ClientMessageSink outbound) override;
    void HandleRequest(api::TransportRequest request) override;
    void Close() noexcept override;

private:
    struct SharedState final {
        std::mutex mutex;
        api::ClientMessageSink outbound;
        std::set<api::RequestId> pending_request_ids;
        api::RequestId last_request_id{0U};
        bool open{false};
    };

    static void SendResponse(
        const std::shared_ptr<SharedState>& state,
        api::RequestId request_id,
        middleware::ValueResult result) noexcept;
    static void SendImmediateResponse(
        const std::shared_ptr<SharedState>& state,
        api::RequestId request_id,
        common::Result<void, api::VehicleError> result) noexcept;
    static bool Send(
        const std::shared_ptr<SharedState>& state,
        api::WireMessage message) noexcept;
    [[nodiscard]] common::Result<api::SessionId, api::VehicleError> ValidateRequestSequence(
        api::RequestId request_id,
        bool reserve_in_flight);

    VehicleService& service_;
    const ClientAccessPolicy& access_policy_;
    const std::chrono::milliseconds request_timeout_;
    const std::size_t maximum_in_flight_;
    const std::shared_ptr<SharedState> state_;
    std::mutex lifecycle_mutex_;
    std::optional<api::SessionId> session_id_;
};

}  // namespace fw03::application
