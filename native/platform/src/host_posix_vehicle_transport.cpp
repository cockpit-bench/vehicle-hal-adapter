#include "fw03/platform/vehicle_transport.h"

#include "fw03/api/wire_codec.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
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
constexpr auto kHandshakeDeadline = std::chrono::seconds{5};
constexpr auto kFrameAssemblyDeadline = std::chrono::seconds{1};
constexpr auto kMaximumBusinessWriteBudget = std::chrono::milliseconds{250};

struct EndpointIdentity final {
    dev_t device{0};
    ino_t inode{0};
    uid_t owner{0};
    gid_t group{0};
    mode_t mode{0};
};

struct ReaderStartGate final {
    std::mutex mutex;
    std::condition_variable available;
    bool start{false};
};

api::VehicleError SocketError(std::string operation, api::RequestId request_id = 0U) {
    const int saved_errno = errno;
    operation.append(": ");
    operation.append(std::strerror(saved_errno));
    return {api::VehicleErrorCode::kTransportDown, std::move(operation), request_id};
}

common::Result<void, api::VehicleError> ConnectWithDeadline(
    int socket_fd,
    const sockaddr_un& address,
    std::chrono::steady_clock::time_point deadline) {
    if (::connect(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0) {
        return common::Result<void, api::VehicleError>::Success();
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        return common::Result<void, api::VehicleError>::Failure(SocketError("connect"));
    }
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle socket connect exceeded its absolute deadline",
                 0U});
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto poll_timeout = static_cast<int>(std::min<std::int64_t>(
            std::numeric_limits<int>::max(),
            std::max<std::int64_t>(1, remaining.count())));
        pollfd descriptor{socket_fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1U, poll_timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle socket connect exceeded its absolute deadline",
                 0U});
        }
        if (ready < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                SocketError("poll vehicle connect"));
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(
                socket_fd,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &socket_error_size) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                SocketError("read vehicle connect result"));
        }
        if (socket_error != 0) {
            errno = socket_error;
            return common::Result<void, api::VehicleError>::Failure(SocketError("connect"));
        }
        return common::Result<void, api::VehicleError>::Success();
    }
}

common::Result<EndpointIdentity, api::VehicleError> InspectTrustedEndpoint(
    const std::string& socket_path,
    std::uint32_t expected_user_id,
    std::uint32_t expected_group_id,
    bool reject_world_writable) {
    struct stat status {};
    if (::lstat(socket_path.c_str(), &status) != 0) {
        return common::Result<EndpointIdentity, api::VehicleError>::Failure(
            SocketError("inspect vehicle socket"));
    }
    if (!S_ISSOCK(status.st_mode)) {
        return common::Result<EndpointIdentity, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "vehicle endpoint is not a UNIX socket", 0U});
    }
    if (static_cast<std::uint32_t>(status.st_uid) != expected_user_id) {
        return common::Result<EndpointIdentity, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "vehicle endpoint owner does not match the configured HAL uid", 0U});
    }
    if (static_cast<std::uint32_t>(status.st_gid) != expected_group_id) {
        return common::Result<EndpointIdentity, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "vehicle endpoint group does not match the configured HAL gid", 0U});
    }
    if (reject_world_writable && (status.st_mode & S_IWOTH) != 0) {
        return common::Result<EndpointIdentity, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "vehicle endpoint must not be world-writable", 0U});
    }
    return common::Result<EndpointIdentity, api::VehicleError>::Success(
        {status.st_dev, status.st_ino, status.st_uid, status.st_gid, status.st_mode});
}

common::Result<void, api::VehicleError> VerifyConnectedPeer(
    int socket_fd,
    std::uint32_t expected_user_id,
    std::uint32_t expected_group_id) {
    // SO_PEERCRED is a Linux-only verification aid contained by the transport adapter.
    // Migrate to the versioned transport identity exchange when every target implements
    // that contract; callers remain independent of this extension.
#ifdef SO_PEERCRED
    struct KernelPeerCredentials final {
        pid_t process_id;
        uid_t user_id;
        gid_t group_id;
    } credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return common::Result<void, api::VehicleError>::Failure(
            SocketError("read vehicle peer credentials"));
    }
    if (length != sizeof(credentials) || credentials.process_id <= 0 ||
        static_cast<std::uint32_t>(credentials.user_id) != expected_user_id ||
        static_cast<std::uint32_t>(credentials.group_id) != expected_group_id) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kPermissionDenied,
             "connected vehicle peer does not match the configured HAL uid/gid", 0U});
    }
    return common::Result<void, api::VehicleError>::Success();
#else
    (void)socket_fd;
    (void)expected_user_id;
    (void)expected_group_id;
    return common::Result<void, api::VehicleError>::Failure(
        {api::VehicleErrorCode::kNotSupported,
         "this POSIX target does not expose authenticated vehicle peer credentials", 0U});
#endif
}

common::Result<void, api::VehicleError> WriteAll(
    int socket_fd,
    const std::uint8_t* data,
    std::size_t size,
    api::RequestId request_id,
    std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle frame write exceeded its absolute deadline",
                 request_id});
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto poll_timeout = static_cast<int>(std::min<std::int64_t>(
            std::numeric_limits<int>::max(),
            std::max<std::int64_t>(1, remaining.count())));
        pollfd descriptor{socket_fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1U, poll_timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle frame write exceeded its absolute deadline",
                 request_id});
        }
        if (ready < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                SocketError("poll vehicle write", request_id));
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "vehicle socket became unavailable during frame write",
                 request_id});
        }
        const auto written = ::send(
            socket_fd,
            data + offset,
            size - offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
    std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle peer did not complete its frame before the deadline",
                 0U});
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto poll_timeout = static_cast<int>(std::min<std::int64_t>(
            std::numeric_limits<int>::max(),
            std::max<std::int64_t>(1, remaining.count())));
        pollfd descriptor{socket_fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1U, poll_timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "vehicle peer did not complete its frame before the deadline",
                 0U});
        }
        if (ready < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                SocketError("poll vehicle frame"));
        }
        const auto received =
            ::recv(socket_fd, data + offset, size - offset, MSG_DONTWAIT);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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
    std::chrono::steady_clock::time_point deadline,
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
        request_id,
        deadline);
    if (!header) {
        return header;
    }
    return WriteAll(socket_fd, payload.data(), payload.size(), request_id, deadline);
}

common::Result<std::vector<std::uint8_t>, api::VehicleError> ReadFrame(
    int socket_fd,
    bool allow_idle) {
    if (allow_idle) {
        for (;;) {
            pollfd descriptor{socket_fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1U, -1);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(
                    SocketError("wait for vehicle frame"));
            }
            break;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          (allow_idle ? kFrameAssemblyDeadline : kHandshakeDeadline);
    std::uint32_t network_length = 0U;
    const auto header = ReadAll(
        socket_fd,
        reinterpret_cast<std::uint8_t*>(&network_length),
        sizeof(network_length),
        deadline);
    if (!header) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(header.error());
    }
    const auto length = ntohl(network_length);
    if (length == 0U || static_cast<std::size_t>(length) > kMaximumFrameBytes) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kTransportDown, "peer sent an invalid vehicle frame length", 0U});
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
    const auto body = ReadAll(socket_fd, payload.data(), payload.size(), deadline);
    if (!body) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(body.error());
    }
    return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Success(std::move(payload));
}

class HostPosixVehicleTransport final : public VehicleTransport {
    class LifecycleOperation final {
    public:
        explicit LifecycleOperation(HostPosixVehicleTransport& owner) : owner_(owner) {
            owner_.BeginLifecycleOperation();
        }

        ~LifecycleOperation() { owner_.EndLifecycleOperation(); }

        LifecycleOperation(const LifecycleOperation&) = delete;
        LifecycleOperation& operator=(const LifecycleOperation&) = delete;

    private:
        HostPosixVehicleTransport& owner_;
    };

public:
    HostPosixVehicleTransport(
        std::string socket_path,
        VehiclePeerTrustPolicy trust_policy,
        VehicleTransportReaderThreadFactory reader_thread_factory)
        : socket_path_(std::move(socket_path)),
          expected_user_id_(trust_policy.expected_user_id.value_or(
              static_cast<std::uint32_t>(::geteuid()))),
          expected_group_id_(trust_policy.expected_group_id.value_or(
              static_cast<std::uint32_t>(::getegid()))),
          reject_world_writable_endpoint_(trust_policy.reject_world_writable_endpoint),
          reader_thread_factory_(
              reader_thread_factory
                  ? std::move(reader_thread_factory)
                  : VehicleTransportReaderThreadFactory{
                        [](std::function<void()> task) {
                            return std::thread(std::move(task));
                        }}) {}

    ~HostPosixVehicleTransport() override { Shutdown(); }

    void SetCallbacks(TransportCallbacks callbacks) override {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_ = std::move(callbacks);
    }

    common::Result<api::ApiVersion, api::VehicleError> Connect(
        const api::ApiVersion& requested_version) override {
        if (IsReaderThread()) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "reconnect must not run on the transport reader callback thread", 0U});
        }
        LifecycleOperation lifecycle_operation(*this);
        if (!StopAndReapOwned()) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal,
                 "failed to reap the previous vehicle transport reader", 0U});
        }

        if (socket_path_.empty() || socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                 {api::VehicleErrorCode::kInvalidArgument, "invalid UNIX socket path", 0U});
        }

        const auto endpoint = InspectTrustedEndpoint(
            socket_path_, expected_user_id_, expected_group_id_, reject_world_writable_endpoint_);
        if (!endpoint) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(endpoint.error());
        }

        const int candidate_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (candidate_fd < 0) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(SocketError("socket"));
        }
        const int descriptor_flags = ::fcntl(candidate_fd, F_GETFD);
        const int status_flags = ::fcntl(candidate_fd, F_GETFL);
        if (descriptor_flags < 0 || status_flags < 0 ||
            ::fcntl(candidate_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
            ::fcntl(candidate_fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
            const auto error = SocketError("configure vehicle socket flags");
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(error);
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1U);
        const auto connected = ConnectWithDeadline(
            candidate_fd,
            address,
            std::chrono::steady_clock::now() + kHandshakeDeadline);
        if (!connected) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                connected.error());
        }
        const auto trusted_peer = VerifyConnectedPeer(
            candidate_fd, expected_user_id_, expected_group_id_);
        if (!trusted_peer) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                trusted_peer.error());
        }
        const auto confirmed_endpoint = InspectTrustedEndpoint(
            socket_path_, expected_user_id_, expected_group_id_, reject_world_writable_endpoint_);
        if (!confirmed_endpoint || confirmed_endpoint.value().device != endpoint.value().device ||
            confirmed_endpoint.value().inode != endpoint.value().inode ||
            confirmed_endpoint.value().owner != endpoint.value().owner ||
            confirmed_endpoint.value().group != endpoint.value().group ||
            confirmed_endpoint.value().mode != endpoint.value().mode) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                confirmed_endpoint
                    ? api::VehicleError{api::VehicleErrorCode::kPermissionDenied,
                                        "vehicle endpoint changed during connection", 0U}
                    : confirmed_endpoint.error());
        }

        const auto hello = api::EncodeWireMessage(api::WireMessage{api::Hello{requested_version}});
        if (!hello) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(hello.error());
        }
        const auto hello_write = WriteFrame(
            candidate_fd,
            hello.value(),
            std::chrono::steady_clock::now() + kHandshakeDeadline);
        if (!hello_write) {
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(hello_write.error());
        }

        const auto ack_frame = ReadFrame(candidate_fd, false);
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
        std::shared_ptr<ReaderStartGate> reader_gate;
        std::thread candidate_reader;
        try {
            reader_gate = std::make_shared<ReaderStartGate>();
            candidate_reader = reader_thread_factory_(
                [this, candidate_fd, reader_gate] {
                    ReaderThreadMain(candidate_fd, reader_gate);
                });
        } catch (...) {
            ::shutdown(candidate_fd, SHUT_RDWR);
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal,
                 "failed to start the vehicle transport reader thread",
                 0U});
        }
        if (!candidate_reader.joinable()) {
            ::shutdown(candidate_fd, SHUT_RDWR);
            ::close(candidate_fd);
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal,
                 "vehicle transport reader factory returned no thread",
                 0U});
        }
        reader_ = std::move(candidate_reader);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            socket_fd_ = candidate_fd;
            connected_ = true;
            stopping_ = false;
            death_notified_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(reader_gate->mutex);
            reader_gate->start = true;
        }
        reader_gate->available.notify_all();
        return compatible;
    }

    common::Result<void, api::VehicleError> Send(
        const api::TransportRequest& request,
        std::chrono::milliseconds timeout) override {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInvalidArgument,
                 "vehicle send requires a positive deadline", request.request_id});
        }
        const auto payload = api::EncodeWireMessage(api::WireMessage{request});
        if (!payload) {
            return common::Result<void, api::VehicleError>::Failure(payload.error());
        }

        common::Result<void, api::VehicleError> result =
            common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown, "vehicle transport is disconnected",
                 request.request_id});
        {
            // Serialize writes with descriptor shutdown, but never hold the state mutex while a
            // potentially blocking peer write is in progress.  Closing under send_mutex_ also
            // prevents a copied descriptor number from being recycled underneath WriteFrame.
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            int socket_fd = -1;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (connected_ && socket_fd_ >= 0) {
                    socket_fd = socket_fd_;
                }
            }
            if (socket_fd >= 0) {
                const auto write_budget = std::min(timeout, kMaximumBusinessWriteBudget);
                result = WriteFrame(
                    socket_fd,
                    payload.value(),
                    std::chrono::steady_clock::now() + write_budget,
                    request.request_id);
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

    void Shutdown() noexcept override { StopConnection(); }

private:
    [[nodiscard]] bool IsReaderThread() const noexcept {
        return active_reader_transport_ == this;
    }

    void BeginLifecycleOperation() {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        lifecycle_available_.wait(
            lock,
            [this] { return !lifecycle_operation_in_progress_; });
        lifecycle_operation_in_progress_ = true;
    }

    void EndLifecycleOperation() noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                lifecycle_operation_in_progress_ = false;
            }
            lifecycle_available_.notify_all();
        } catch (...) {
            // A failed host synchronization primitive cannot be recovered here.  The
            // connection has already been signalled down; never let a noexcept shutdown
            // escape through a destructor.
        }
    }

    void ReaderThreadMain(
        int socket_fd,
        const std::shared_ptr<ReaderStartGate>& reader_gate) noexcept {
        const auto* const previous_reader_transport = active_reader_transport_;
        active_reader_transport_ = this;
        try {
            {
                std::unique_lock<std::mutex> lock(reader_gate->mutex);
                reader_gate->available.wait(
                    lock,
                    [reader_gate] { return reader_gate->start; });
            }
            ReaderLoop(socket_fd);
        } catch (...) {
            MarkDeadFromException("vehicle transport reader thread threw an exception");
        }
        active_reader_transport_ = previous_reader_transport;
    }

    void ReaderLoop(int socket_fd) {
        for (;;) {
            const auto frame = ReadFrame(socket_fd, true);
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
                    try {
                        callbacks.on_response(*response);
                    } catch (...) {
                        MarkDeadFromException("vehicle response callback threw an exception");
                        return;
                    }
                }
            } else if (const auto* event = std::get_if<api::PropertyEvent>(&decoded.value())) {
                if (callbacks.on_event) {
                    try {
                        callbacks.on_event(*event);
                    } catch (...) {
                        MarkDeadFromException("vehicle event callback threw an exception");
                        return;
                    }
                }
            } else {
                MarkDead({api::VehicleErrorCode::kTransportDown,
                          "unexpected message after capability negotiation", 0U});
                return;
            }
        }
    }

    void MarkDead(api::VehicleError error) noexcept {
        try {
            std::function<void(api::VehicleError)> death_callback;
            {
                // Keep the same send -> state lock order used by Send and StopConnection.  Only
                // shutdown the descriptor here; an external lifecycle owner closes it after the
                // reader exits.
                std::lock_guard<std::mutex> send_lock(send_mutex_);
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!connected_) {
                    return;
                }
                connected_ = false;
                if (socket_fd_ >= 0) {
                    ::shutdown(socket_fd_, SHUT_RDWR);
                }
                if (!stopping_ && !death_notified_) {
                    death_notified_ = true;
                    std::lock_guard<std::mutex> callbacks_lock(callbacks_mutex_);
                    death_callback = callbacks_.on_death;
                }
            }
            if (death_callback) {
                try {
                    death_callback(std::move(error));
                } catch (...) {
                    // User callbacks are isolation boundaries.  The transport is already dead;
                    // a callback failure must not terminate the reader thread or the process.
                }
            }
        } catch (...) {
            ForceDeadWithoutCallback();
        }
    }

    void MarkDeadFromException(const char* detail) noexcept {
        try {
            MarkDead({api::VehicleErrorCode::kInternal, detail, 0U});
        } catch (...) {
            // Constructing the diagnostic itself may fail under memory pressure.  Preserve the
            // fail-closed state transition even when no callback diagnostic can be delivered.
            ForceDeadWithoutCallback();
        }
    }

    void ForceDeadWithoutCallback() noexcept {
        try {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            std::lock_guard<std::mutex> lock(state_mutex_);
            stopping_ = true;
            connected_ = false;
            if (socket_fd_ >= 0) {
                ::shutdown(socket_fd_, SHUT_RDWR);
            }
            death_notified_ = true;
        } catch (...) {
            // Best effort only: this path is already handling a synchronization/allocation
            // failure and must remain noexcept.
        }
    }

    void SignalStop() noexcept {
        try {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            std::lock_guard<std::mutex> lock(state_mutex_);
            stopping_ = true;
            connected_ = false;
            if (socket_fd_ >= 0) {
                ::shutdown(socket_fd_, SHUT_RDWR);
            }
        } catch (...) {
            ForceDeadWithoutCallback();
        }
    }

    [[nodiscard]] bool StopAndReapOwned() noexcept {
        SignalStop();
        std::thread stopped_reader = std::move(reader_);
        if (stopped_reader.joinable()) {
            try {
                stopped_reader.join();
            } catch (...) {
                // Preserve ownership so a later external lifecycle operation can retry.  This
                // branch is not expected for a valid non-self std::thread, but dropping a
                // joinable thread would call std::terminate.
                reader_ = std::move(stopped_reader);
                return false;
            }
        }
        try {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (socket_fd_ >= 0) {
                ::close(socket_fd_);
                socket_fd_ = -1;
            }
        } catch (...) {
            ForceDeadWithoutCallback();
            return false;
        }
        return true;
    }

    void StopConnection() noexcept {
        // Reader-owned callbacks may request shutdown while an external owner is already
        // joining this exact reader.  They only signal the descriptor and return; the external
        // owner is solely responsible for moving/joining/closing the reader generation.
        SignalStop();
        if (IsReaderThread()) {
            return;
        }
        try {
            LifecycleOperation lifecycle_operation(*this);
            // Repeat after ownership acquisition: a concurrent Connect may have installed a new
            // descriptor while this caller was waiting for lifecycle ownership.
            (void)StopAndReapOwned();
        } catch (...) {
            ForceDeadWithoutCallback();
        }
    }

    const std::string socket_path_;
    const std::uint32_t expected_user_id_;
    const std::uint32_t expected_group_id_;
    const bool reject_world_writable_endpoint_;
    VehicleTransportReaderThreadFactory reader_thread_factory_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_available_;
    bool lifecycle_operation_in_progress_{false};
    mutable std::mutex state_mutex_;
    std::mutex callbacks_mutex_;
    std::mutex send_mutex_;
    TransportCallbacks callbacks_;
    std::thread reader_;
    int socket_fd_{-1};
    bool connected_{false};
    bool stopping_{true};
    bool death_notified_{false};
    inline static thread_local const HostPosixVehicleTransport* active_reader_transport_{nullptr};
};

}  // namespace

std::shared_ptr<VehicleTransport> CreateHostPosixVehicleTransport(
    std::string socket_path,
    VehiclePeerTrustPolicy trust_policy,
    VehicleTransportReaderThreadFactory reader_thread_factory) {
    return std::make_shared<HostPosixVehicleTransport>(
        std::move(socket_path),
        std::move(trust_policy),
        std::move(reader_thread_factory));
}

}  // namespace fw03::platform
