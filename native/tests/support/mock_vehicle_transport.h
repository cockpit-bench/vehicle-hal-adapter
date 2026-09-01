#pragma once

#include "fw03/platform/vehicle_transport.h"

#include <gmock/gmock.h>

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace fw03::test {

class MockVehicleTransport : public platform::VehicleTransport {
public:
    MockVehicleTransport() {
        using testing::_;
        ON_CALL(*this, SetCallbacks(_))
            .WillByDefault([this](platform::TransportCallbacks callbacks) {
                std::lock_guard<std::mutex> lock(mutex_);
                callbacks_ = std::move(callbacks);
            });
        ON_CALL(*this, Connect(_))
            .WillByDefault([this](const api::ApiVersion& requested) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++connect_calls_;
                const auto negotiated = api::NegotiateApiVersion(requested, peer_version_);
                connected_ = negotiated.ok();
                return negotiated;
            });
        ON_CALL(*this, Send(_))
            .WillByDefault([this](const api::TransportRequest& request) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_) {
                    return common::Result<void, api::VehicleError>::Failure(
                        {api::VehicleErrorCode::kTransportDown, "mock peer is disconnected",
                         request.request_id});
                }
                if (next_send_error_.has_value()) {
                    auto error = std::move(*next_send_error_);
                    next_send_error_.reset();
                    return common::Result<void, api::VehicleError>::Failure(std::move(error));
                }
                requests_.push_back(request);
                return common::Result<void, api::VehicleError>::Success();
            });
        ON_CALL(*this, IsConnected())
            .WillByDefault([this] {
                std::lock_guard<std::mutex> lock(mutex_);
                return connected_;
            });
        ON_CALL(*this, Shutdown())
            .WillByDefault([this] {
                std::lock_guard<std::mutex> lock(mutex_);
                connected_ = false;
            });
    }

    MOCK_METHOD(void, SetCallbacks, (platform::TransportCallbacks callbacks), (override));
    MOCK_METHOD(
        (common::Result<api::ApiVersion, api::VehicleError>),
        Connect,
        (const api::ApiVersion& requested_version),
        (override));
    MOCK_METHOD(
        (common::Result<void, api::VehicleError>),
        Send,
        (const api::TransportRequest& request),
        (override));
    MOCK_METHOD(bool, IsConnected, (), (const, noexcept, override));
    MOCK_METHOD(void, Shutdown, (), (noexcept, override));

    [[nodiscard]] std::vector<api::TransportRequest> Requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    void ClearRequests() {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

    [[nodiscard]] std::size_t ConnectCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_calls_;
    }

    void SetPeerVersion(api::ApiVersion version) {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_version_ = version;
    }

    void FailNextSend(api::VehicleError error) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_send_error_ = std::move(error);
    }

    void EmitResponse(api::TransportResponse response) {
        std::function<void(api::TransportResponse)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callbacks_.on_response;
        }
        if (callback) {
            callback(std::move(response));
        }
    }

    void EmitEvent(api::PropertyEvent event) {
        std::function<void(api::PropertyEvent)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callbacks_.on_event;
        }
        if (callback) {
            callback(std::move(event));
        }
    }

    void EmitDeath(api::VehicleError error = {
                       api::VehicleErrorCode::kTransportDown, "mock transport died", 0U}) {
        std::function<void(api::VehicleError)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            callback = callbacks_.on_death;
        }
        if (callback) {
            callback(std::move(error));
        }
    }

private:
    mutable std::mutex mutex_;
    platform::TransportCallbacks callbacks_;
    std::vector<api::TransportRequest> requests_;
    std::optional<api::VehicleError> next_send_error_;
    api::ApiVersion peer_version_{api::CurrentApiVersion()};
    std::size_t connect_calls_{0U};
    bool connected_{false};
};

}  // namespace fw03::test
