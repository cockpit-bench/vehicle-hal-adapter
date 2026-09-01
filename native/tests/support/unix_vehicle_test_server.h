#pragma once

#include "fw03/api/wire_codec.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::test {

class UnixVehicleTestServer final {
public:
    UnixVehicleTestServer() {
        static std::atomic<std::uint32_t> counter{0U};
        path_ = "/tmp/fw03-vehicle-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(counter.fetch_add(1U)) + ".sock";
    }

    ~UnixVehicleTestServer() { Stop(); }

    UnixVehicleTestServer(const UnixVehicleTestServer&) = delete;
    UnixVehicleTestServer& operator=(const UnixVehicleTestServer&) = delete;

    [[nodiscard]] bool Start() {
        ::unlink(path_.c_str());
        const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) {
            SetError("socket failed");
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) {
            ::close(listener);
            SetError("socket path too long");
            return false;
        }
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener, 1) != 0) {
            ::close(listener);
            SetError("bind or listen failed");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listener_fd_ = listener;
            stopping_ = false;
        }
        worker_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            CloseSocket(client_fd_);
            CloseSocket(listener_fd_);
        }
        requests_available_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        ::unlink(path_.c_str());
    }

    void DisconnectClient() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseSocket(client_fd_);
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    [[nodiscard]] std::vector<api::TransportRequest> Requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    [[nodiscard]] bool WaitForRequests(
        std::size_t count,
        std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return requests_available_.wait_for(
            lock,
            timeout,
            [this, count] { return requests_.size() >= count || !error_.empty(); });
    }

    [[nodiscard]] bool SendEvent(api::PropertyEvent event) {
        return SendMessage(api::WireMessage{std::move(event)});
    }

private:
    static void CloseSocket(int& socket_fd) noexcept {
        if (socket_fd >= 0) {
            ::shutdown(socket_fd, SHUT_RDWR);
            ::close(socket_fd);
            socket_fd = -1;
        }
    }

    static bool ReadAll(int socket_fd, std::uint8_t* data, std::size_t size) {
        std::size_t offset = 0U;
        while (offset < size) {
            const auto received = ::recv(socket_fd, data + offset, size - offset, 0);
            if (received <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    static bool WriteAll(int socket_fd, const std::uint8_t* data, std::size_t size) {
        std::size_t offset = 0U;
        while (offset < size) {
            const auto written = ::send(socket_fd, data + offset, size - offset, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    static bool ReadFrame(int socket_fd, std::vector<std::uint8_t>& payload) {
        std::uint32_t network_length = 0U;
        if (!ReadAll(
                socket_fd,
                reinterpret_cast<std::uint8_t*>(&network_length),
                sizeof(network_length))) {
            return false;
        }
        const auto length = ntohl(network_length);
        if (length == 0U || length > (1024U * 1024U) + 256U) {
            return false;
        }
        payload.resize(static_cast<std::size_t>(length));
        return ReadAll(socket_fd, payload.data(), payload.size());
    }

    static bool WriteFrame(int socket_fd, const std::vector<std::uint8_t>& payload) {
        const auto network_length = htonl(static_cast<std::uint32_t>(payload.size()));
        return WriteAll(
                   socket_fd,
                   reinterpret_cast<const std::uint8_t*>(&network_length),
                   sizeof(network_length)) &&
               WriteAll(socket_fd, payload.data(), payload.size());
    }

    [[nodiscard]] bool SendMessage(api::WireMessage message) {
        const auto encoded = api::EncodeWireMessage(message);
        if (!encoded) {
            SetError(encoded.error().detail);
            return false;
        }
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        int client = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client = client_fd_;
        }
        if (client < 0 || !WriteFrame(client, encoded.value())) {
            SetError("server write failed");
            return false;
        }
        return true;
    }

    void Run() {
        int listener = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listener = listener_fd_;
        }
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (!IsStopping()) {
                SetError("accept failed");
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client_fd_ = client;
        }

        std::vector<std::uint8_t> frame;
        if (!ReadFrame(client, frame)) {
            SetError("failed to read capability hello");
            return;
        }
        const auto decoded_hello = api::DecodeWireMessage(frame);
        if (!decoded_hello || !std::holds_alternative<api::Hello>(decoded_hello.value())) {
            SetError("invalid capability hello");
            return;
        }
        const auto& requested = std::get<api::Hello>(decoded_hello.value()).requested_version;
        const auto negotiated = api::NegotiateApiVersion(requested, api::CurrentApiVersion());
        api::HelloAck ack;
        if (negotiated) {
            ack.negotiated_version = negotiated.value();
            ack.error = {api::VehicleErrorCode::kOk, {}, 0U};
        } else {
            ack.error = negotiated.error();
        }
        if (!SendMessage(api::WireMessage{ack}) || !negotiated) {
            return;
        }

        while (!IsStopping()) {
            frame.clear();
            if (!ReadFrame(client, frame)) {
                if (!IsStopping()) {
                    // A deliberate client disconnect is a normal server exit in death tests.
                }
                return;
            }
            const auto decoded = api::DecodeWireMessage(frame);
            if (!decoded || !std::holds_alternative<api::TransportRequest>(decoded.value())) {
                SetError("invalid transport request");
                return;
            }
            const auto request = std::get<api::TransportRequest>(decoded.value());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            requests_available_.notify_all();

            api::TransportResponse response;
            response.request_id = request.request_id;
            response.error = {api::VehicleErrorCode::kOk, {}, request.request_id};
            if (request.operation == api::TransportOperation::kGet) {
                response.value = api::VehiclePropertyValue{
                    request.key,
                    1234000,
                    api::PropertyStatus::kAvailable,
                    std::int32_t{77}};
            }
            if (!SendMessage(api::WireMessage{response})) {
                return;
            }
        }
    }

    [[nodiscard]] bool IsStopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    void SetError(std::string error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_.empty()) {
                error_ = std::move(error);
            }
        }
        requests_available_.notify_all();
    }

    std::string path_;
    mutable std::mutex mutex_;
    std::mutex write_mutex_;
    std::condition_variable requests_available_;
    std::thread worker_;
    std::vector<api::TransportRequest> requests_;
    std::string error_;
    int listener_fd_{-1};
    int client_fd_{-1};
    bool stopping_{true};
};

}  // namespace fw03::test
