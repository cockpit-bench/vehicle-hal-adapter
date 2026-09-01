#include "fw03/platform/vehicle_transport.h"

#include "fw03/api/wire_codec.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::platform {
namespace {

constexpr std::size_t kMaximumFrameBytes = (1024U * 1024U) + 256U;

api::VehicleError SocketError(std::string operation, api::RequestId request_id = 0U) {
    operation.append(": ");
    operation.append(std::strerror(errno));
    return {api::VehicleErrorCode::kTransportDown, std::move(operation), request_id};
}

common::Result<void, api::VehicleError> WriteAll(
    int socket_fd,
    const std::uint8_t* data,
    std::size_t size,
    api::RequestId request_id) {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto written = ::send(
            socket_fd,
            data + offset,
            size - offset,
            MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return common::Result<void, api::VehicleError>::Failure(
                SocketError("send", request_id));
        }
        offset += static_cast<std::size_t>(written);
    }
    return common::Result<void, api::VehicleError>::Success();
}

common::Result<void, api::VehicleError> ReadAll(
    int socket_fd,
    std::uint8_t* data,
    std::size_t size) {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto received = ::recv(socket_fd, data + offset, size - offset, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown, "peer closed the vehicle socket", 0U});
        }
        if (received < 0) {
            return common::Result<void, api::VehicleError>::Failure(SocketError("recv"));
        }
        offset += static_cast<std::size_t>(received);
    }
    return common::Result<void, api::VehicleError>::Success();
}

common::Result<void, api::VehicleError> WriteFrame(
    int socket_fd,
    const std::vector<std::uint8_t>& payload,
    api::RequestId request_id = 0U) {
    if (payload.empty() || payload.size() > kMaximumFrameBytes ||
        payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "invalid vehicle frame length", request_id});
    }
    const auto network_length = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto header = WriteAll(
        socket_fd,
        reinterpret_cast<const std::uint8_t*>(&network_length),
        sizeof(network_length),
        request_id);
    if (!header) {
        return header;
    }
    return WriteAll(socket_fd, payload.data(), payload.size(), request_id);
}

common::Result<std::vector<std::uint8_t>, api::VehicleError> ReadFrame(int socket_fd) {
    std::uint32_t network_length = 0U;
    const auto header = ReadAll(
        socket_fd,
        reinterpret_cast<std::uint8_t*>(&network_length),
        sizeof(network_length));
    if (!header) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(header.error());
    }
    const auto length = ntohl(network_length);
    if (length == 0U || static_cast<std::size_t>(length) > kMaximumFrameBytes) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kTransportDown, "peer sent an invalid vehicle frame length", 0U});
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
    const auto body = ReadAll(socket_fd, payload.data(), payload.size());
    if (!body) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(body.error());
    }
    return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Success(std::move(payload));
}

class HostPosixVehicleTransport final : public VehicleTransport {
public:
    explicit HostPosixVehicleTransport(std::string socket_path)
        : socket_path_(std::move(socket_path)) {}

    ~HostPosixVehicleTransport() override { Shutdown(); }

    void SetCallbacks(TransportCallbacks callbacks) override {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_ = std::move(callbacks);
    }

    common::Result<api::ApiVersion, api::VehicleError> Connect(
        const api::ApiVersion& requested_version) override {
        if (reader_.joinable() && reader_.get_id() == std::this_thread::get_id()) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "reconnect must not run on the transport reader callback thread", 0U});
        }
        StopConnection(false);

        if (socket_path_.empty() || socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInvalidArgument, "invalid UNIX socket path", 0U});
        }

        const int candidate_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (candidate_fd < 0) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(SocketError("socket"));
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1U);
        if (::connect(candidate_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const auto error = SocketError("connect");
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(error);
        }

        const auto hello = api::EncodeWireMessage(api::WireMessage{api::Hello{requested_version}});
        if (!hello) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(hello.error());
        }
        const auto hello_write = WriteFrame(candidate_fd, hello.value());
        if (!hello_write) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(hello_write.error());
        }

        const auto ack_frame = ReadFrame(candidate_fd);
        if (!ack_frame) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(ack_frame.error());
        }
        const auto decoded = api::DecodeWireMessage(ack_frame.value());
        if (!decoded || !std::holds_alternative<api::HelloAck>(decoded.value())) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                decoded ? api::VehicleError{api::VehicleErrorCode::kTransportDown,
                                            "vehicle peer did not return a capability ack", 0U}
                        : decoded.error());
        }
        const auto& ack = std::get<api::HelloAck>(decoded.value());
        if (ack.error.code != api::VehicleErrorCode::kOk) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(ack.error);
        }
        const auto compatible = api::NegotiateApiVersion(requested_version, ack.negotiated_version);
        if (!compatible) {
            ::close(candidate_fd);
            return compatible;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            socket_fd_ = candidate_fd;
            connected_ = true;
            stopping_ = false;
            death_notified_ = false;
        }
        reader_ = std::thread([this, candidate_fd] { ReaderLoop(candidate_fd); });
        return compatible;
    }

    common::Result<void, api::VehicleError> Send(
        const api::TransportRequest& request) override {
        const auto payload = api::EncodeWireMessage(api::WireMessage{request});
        if (!payload) {
            return common::Result<void, api::VehicleError>::Failure(payload.error());
        }

        std::lock_guard<std::mutex> send_lock(send_mutex_);
        common::Result<void, api::VehicleError> result =
            common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown, "vehicle transport is disconnected",
                 request.request_id});
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (connected_ && socket_fd_ >= 0) {
                result = WriteFrame(socket_fd_, payload.value(), request.request_id);
            }
        }
        if (!result) {
            MarkDead(result.error());
        }
        return result;
    }

    [[nodiscard]] bool IsConnected() const noexcept override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return connected_;
    }

    void Shutdown() noexcept override { StopConnection(false); }

private:
    void ReaderLoop(int socket_fd) {
        for (;;) {
            const auto frame = ReadFrame(socket_fd);
            if (!frame) {
                MarkDead(frame.error());
                return;
            }
            const auto decoded = api::DecodeWireMessage(frame.value());
            if (!decoded) {
                MarkDead({api::VehicleErrorCode::kTransportDown,
                          "invalid vehicle protocol frame: " + decoded.error().detail, 0U});
                return;
            }

            TransportCallbacks callbacks;
            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                callbacks = callbacks_;
            }
            if (const auto* response = std::get_if<api::TransportResponse>(&decoded.value())) {
                if (callbacks.on_response) {
                    callbacks.on_response(*response);
                }
            } else if (const auto* event = std::get_if<api::PropertyEvent>(&decoded.value())) {
                if (callbacks.on_event) {
                    callbacks.on_event(*event);
                }
            } else {
                MarkDead({api::VehicleErrorCode::kTransportDown,
                          "unexpected message after capability negotiation", 0U});
                return;
            }
        }
    }

    void MarkDead(api::VehicleError error) noexcept {
        std::function<void(api::VehicleError)> death_callback;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!connected_ && socket_fd_ < 0) {
                return;
            }
            connected_ = false;
            if (socket_fd_ >= 0) {
                ::shutdown(socket_fd_, SHUT_RDWR);
                ::close(socket_fd_);
                socket_fd_ = -1;
            }
            if (!stopping_ && !death_notified_) {
                death_notified_ = true;
                std::lock_guard<std::mutex> callbacks_lock(callbacks_mutex_);
                death_callback = callbacks_.on_death;
            }
        }
        if (death_callback) {
            death_callback(std::move(error));
        }
    }

    void StopConnection(bool notify_death) noexcept {
        std::function<void(api::VehicleError)> death_callback;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const bool was_connected = connected_;
            stopping_ = true;
            connected_ = false;
            if (socket_fd_ >= 0) {
                ::shutdown(socket_fd_, SHUT_RDWR);
                ::close(socket_fd_);
                socket_fd_ = -1;
            }
            if (notify_death && was_connected && !death_notified_) {
                death_notified_ = true;
                std::lock_guard<std::mutex> callbacks_lock(callbacks_mutex_);
                death_callback = callbacks_.on_death;
            }
        }
        if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
            reader_.join();
        }
        if (death_callback) {
            death_callback({api::VehicleErrorCode::kTransportDown,
                            "vehicle transport stopped", 0U});
        }
    }

    const std::string socket_path_;
    mutable std::mutex state_mutex_;
    std::mutex callbacks_mutex_;
    std::mutex send_mutex_;
    TransportCallbacks callbacks_;
    std::thread reader_;
    int socket_fd_{-1};
    bool connected_{false};
    bool stopping_{true};
    bool death_notified_{false};
};

}  // namespace

std::shared_ptr<VehicleTransport> CreateHostPosixVehicleTransport(std::string socket_path) {
    return std::make_shared<HostPosixVehicleTransport>(std::move(socket_path));
}

}  // namespace fw03::platform
