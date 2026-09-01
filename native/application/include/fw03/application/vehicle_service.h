#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_contract.h"
#include "fw03/middleware/vehicle_property_gateway.h"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fw03::application {

struct CallerContext final {
    std::string client_name;
    std::set<std::uint32_t> readable_properties;
    std::set<std::uint32_t> writable_properties;
};

struct SessionCallbacks final {
    std::function<void(api::PropertyEvent)> on_property_event;
    std::function<void(bool, api::VehicleError)> on_transport_state;
};

class VehicleService final {
public:
    explicit VehicleService(middleware::VehiclePropertyGateway& gateway);
    ~VehicleService();

    VehicleService(const VehicleService&) = delete;
    VehicleService& operator=(const VehicleService&) = delete;

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Start();

    [[nodiscard]] common::Result<api::SessionId, api::VehicleError> OpenSession(
        CallerContext caller,
        api::ApiVersion requested_version,
        SessionCallbacks callbacks);

    [[nodiscard]] common::Result<void, api::VehicleError> CloseSession(
        api::SessionId session_id);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Get(
        api::SessionId session_id,
        api::PropertyKey key,
        std::chrono::milliseconds timeout,
        middleware::ValueCompletion completion,
        bool prefer_cache = true);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Set(
        api::SessionId session_id,
        api::VehiclePropertyValue value,
        std::chrono::milliseconds timeout,
        middleware::ValueCompletion completion);

    [[nodiscard]] common::Result<void, api::VehicleError> Subscribe(
        api::SessionId session_id,
        api::PropertyKey key,
        float sample_rate_hz);

    [[nodiscard]] common::Result<void, api::VehicleError> Unsubscribe(
        api::SessionId session_id,
        api::PropertyKey key);

    void PollTimeouts();
    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Reconnect();
    [[nodiscard]] bool IsConnected() const noexcept;
    void Shutdown() noexcept;

private:
    struct Session final {
        CallerContext caller;
        api::ApiVersion negotiated_version;
        SessionCallbacks callbacks;
        std::set<api::PropertyKey> subscriptions;
    };

    [[nodiscard]] common::Result<void, api::VehicleError> CheckAccess(
        api::SessionId session_id,
        std::uint32_t property_id,
        bool write) const;
    void DispatchEvent(api::SessionId session_id, api::PropertyEvent event);
    void DispatchTransportState(bool connected, api::VehicleError error);

    middleware::VehiclePropertyGateway& gateway_;
    mutable std::mutex mutex_;
    std::map<api::SessionId, Session> sessions_;
    api::SessionId next_session_id_{1U};
    bool started_{false};
    bool shutdown_{false};
};

}  // namespace fw03::application
