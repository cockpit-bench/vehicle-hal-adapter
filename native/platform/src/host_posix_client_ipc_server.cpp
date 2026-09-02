#include "fw03/platform/client_ipc_server.h"

#include "fw03/api/wire_codec.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::platform {
namespace {

constexpr std::size_t kMaximumFrameBytes = (1024U * 1024U) + 256U;
constexpr auto kFrameAssemblyDeadline = std::chrono::seconds{5};
constexpr auto kClientWriteDeadline = std::chrono::milliseconds{100};
constexpr auto kEndpointProbeDeadline = std::chrono::milliseconds{100};
constexpr mode_t kEndpointMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
constexpr mode_t kLeaseMode = S_IRUSR | S_IWUSR;

thread_local const void* g_active_client_server_thread = nullptr;

class ScopedClientServerThread final {
public:
    explicit ScopedClientServerThread(const void* server) noexcept
        : previous_(g_active_client_server_thread) {
        g_active_client_server_thread = server;
    }

    ~ScopedClientServerThread() { g_active_client_server_thread = previous_; }

    ScopedClientServerThread(const ScopedClientServerThread&) = delete;
    ScopedClientServerThread& operator=(const ScopedClientServerThread&) = delete;

private:
    const void* previous_;
};

class OwnedFileDescriptor final {
public:
    OwnedFileDescriptor() = default;
    explicit OwnedFileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}

    ~OwnedFileDescriptor() { Reset(); }

    OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;

    OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept
        : descriptor_(other.Release()) {}

    OwnedFileDescriptor& operator=(OwnedFileDescriptor&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] int Get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

    [[nodiscard]] int Release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

    void Reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_{-1};
};

enum class EndpointProbeResult {
    kActive,
    kStale,
    kIndeterminate,
};

[[nodiscard]] mode_t PermissionBits(mode_t mode) noexcept {
    return mode & (S_IRWXU | S_IRWXG | S_IRWXO);
}

[[nodiscard]] bool SameInode(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

[[nodiscard]] bool IsTrustedEndpoint(const struct stat& status) noexcept {
    const auto permissions = PermissionBits(status.st_mode);
    const bool group_write_is_trusted =
        (permissions & S_IWGRP) == 0 || status.st_gid == ::getegid();
    return S_ISSOCK(status.st_mode) && status.st_uid == ::geteuid() &&
           status.st_nlink == 1 && (permissions & S_IWUSR) != 0 &&
           (permissions & S_IWOTH) == 0 && group_write_is_trusted &&
           (status.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) == 0;
}

[[nodiscard]] bool IsTrustedLease(const struct stat& status) noexcept {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           status.st_nlink == 1 && PermissionBits(status.st_mode) == kLeaseMode &&
           (status.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) == 0;
}

void ShutdownSocket(OwnedFileDescriptor& socket) noexcept {
    if (socket) {
        ::shutdown(socket.Get(), SHUT_RDWR);
        socket.Reset();
    }
}

api::VehicleError PosixError(std::string operation, api::RequestId request_id = 0U) {
    const int saved_errno = errno;
    operation.append(": ");
    operation.append(std::strerror(saved_errno));
    return {api::VehicleErrorCode::kTransportDown, std::move(operation), request_id};
}

[[nodiscard]] std::string ParentPath(const std::string& socket_path) {
    const auto separator = socket_path.find_last_of('/');
    if (separator == std::string::npos) {
        return {};
    }
    return separator == 0U ? "/" : socket_path.substr(0U, separator);
}

[[nodiscard]] std::string EndpointName(const std::string& socket_path) {
    const auto separator = socket_path.find_last_of('/');
    if (separator == std::string::npos || separator + 1U >= socket_path.size()) {
        return {};
    }
    return socket_path.substr(separator + 1U);
}

[[nodiscard]] EndpointProbeResult ProbeEndpoint(const std::string& socket_path) noexcept {
    OwnedFileDescriptor probe{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!probe) {
        return EndpointProbeResult::kIndeterminate;
    }
    const int descriptor_flags = ::fcntl(probe.Get(), F_GETFD);
    const int status_flags = ::fcntl(probe.Get(), F_GETFL);
    if (descriptor_flags < 0 || status_flags < 0 ||
        ::fcntl(probe.Get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
        ::fcntl(probe.Get(), F_SETFL, status_flags | O_NONBLOCK) != 0) {
        return EndpointProbeResult::kIndeterminate;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    if (::connect(
            probe.Get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0) {
        return EndpointProbeResult::kActive;
    }
    const int connect_error = errno;
    if (connect_error == ECONNREFUSED) {
        return EndpointProbeResult::kStale;
    }
    if (connect_error != EINPROGRESS && connect_error != EALREADY &&
        connect_error != EAGAIN && connect_error != EINTR) {
        return EndpointProbeResult::kIndeterminate;
    }

    const auto deadline = std::chrono::steady_clock::now() + kEndpointProbeDeadline;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return EndpointProbeResult::kIndeterminate;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto poll_timeout = static_cast<int>(std::min<std::int64_t>(
            std::numeric_limits<int>::max(),
            std::max<std::int64_t>(1, remaining.count())));
        pollfd descriptor{probe.Get(), POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1U, poll_timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return EndpointProbeResult::kIndeterminate;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (::getsockopt(
                probe.Get(),
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length) != 0 ||
            error_length != sizeof(socket_error)) {
            return EndpointProbeResult::kIndeterminate;
        }
        if (socket_error == 0 || socket_error == EISCONN) {
            return EndpointProbeResult::kActive;
        }
        return socket_error == ECONNREFUSED
                   ? EndpointProbeResult::kStale
                   : EndpointProbeResult::kIndeterminate;
    }
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
                 "client frame write exceeded its absolute deadline",
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
                 "client frame write exceeded its absolute deadline",
                 request_id});
        }
        if (ready < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("poll client frame write", request_id));
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "client socket became unavailable during frame write",
                 request_id});
        }
        const auto written =
            ::send(socket_fd, data + offset, size - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (written <= 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("send client frame", request_id));
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
                 "client did not complete its IPC frame before the deadline", 0U});
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd descriptor{socket_fd, POLLIN, 0};
        const int ready = ::poll(
            &descriptor,
            1U,
            static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTimeout,
                 "client did not complete its IPC frame before the deadline", 0U});
        }
        if (ready < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("poll client frame"));
        }
        const auto received = ::recv(
            socket_fd,
            data + offset,
            size - offset,
            MSG_DONTWAIT);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (received == 0) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown, "client closed the IPC socket", 0U});
        }
        if (received < 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("receive client frame"));
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
            {api::VehicleErrorCode::kInvalidArgument, "invalid client frame length", request_id});
    }
    const auto network_length = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto deadline = std::chrono::steady_clock::now() + kClientWriteDeadline;
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
        pollfd descriptor{socket_fd, POLLIN, 0};
        for (;;) {
            const int ready = ::poll(&descriptor, 1U, -1);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(
                    PosixError("wait for client frame"));
            }
            break;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + kFrameAssemblyDeadline;
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
            {api::VehicleErrorCode::kTransportDown, "client sent an invalid frame length", 0U});
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
    const auto body = ReadAll(socket_fd, payload.data(), payload.size(), deadline);
    if (!body) {
        return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Failure(body.error());
    }
    return common::Result<std::vector<std::uint8_t>, api::VehicleError>::Success(std::move(payload));
}

class ClientConnection final {
public:
    explicit ClientConnection(OwnedFileDescriptor socket) noexcept
        : socket_(std::move(socket)) {}

    ~ClientConnection() { Finalize(); }

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    [[nodiscard]] common::Result<std::vector<std::uint8_t>, api::VehicleError> Read(
        bool allow_idle) const {
        return ReadFrame(socket_.Get(), allow_idle);
    }

    [[nodiscard]] bool Send(api::WireMessage message) noexcept {
        try {
            const auto payload = api::EncodeWireMessage(message);
            if (!payload) {
                return false;
            }
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (!active_ || !socket_) {
                return false;
            }
            const auto request_id =
                std::holds_alternative<api::TransportResponse>(message)
                    ? std::get<api::TransportResponse>(message).request_id
                    : 0U;
            const auto written = WriteFrame(socket_.Get(), payload.value(), request_id);
            if (!written) {
                active_ = false;
                ::shutdown(socket_.Get(), SHUT_RDWR);
                return false;
            }
            return true;
        } catch (...) {
            Interrupt();
            return false;
        }
    }

    void Interrupt() noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (socket_) {
            active_ = false;
            ::shutdown(socket_.Get(), SHUT_RDWR);
        }
    }

    void Finalize() noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        active_ = false;
        ShutdownSocket(socket_);
    }

private:
    mutable std::mutex write_mutex_;
    OwnedFileDescriptor socket_;
    bool active_{true};
};

class HostPosixClientIpcServer final : public VehicleClientIpcServer {
public:
    HostPosixClientIpcServer(
        std::string socket_path,
        api::ClientRequestSessionFactory session_factory,
        std::size_t maximum_clients,
        std::size_t maximum_clients_per_user,
        ClientWorkerThreadFactory worker_thread_factory)
        : socket_path_(std::move(socket_path)),
          parent_path_(ParentPath(socket_path_)),
          endpoint_name_(EndpointName(socket_path_)),
          lease_name_(endpoint_name_ + ".lease"),
          session_factory_(std::move(session_factory)),
          maximum_clients_(maximum_clients),
          maximum_clients_per_user_(std::min(maximum_clients, maximum_clients_per_user)),
          worker_thread_factory_(
              worker_thread_factory
                  ? std::move(worker_thread_factory)
                  : ClientWorkerThreadFactory{
                        [](std::function<void()> task) {
                            return std::thread(std::move(task));
                        }}) {}

    ~HostPosixClientIpcServer() override { Shutdown(); }

    common::Result<void, api::VehicleError> Start() override {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (shutdown_in_progress_) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "client IPC shutdown is still in progress", 0U});
        }
        if (running_.load()) {
            return common::Result<void, api::VehicleError>::Success();
        }
        if (!session_factory_ || maximum_clients_ == 0U || maximum_clients_per_user_ == 0U ||
            socket_path_.empty() || parent_path_.empty() || endpoint_name_.empty() ||
            socket_path_.front() != '/' || socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInvalidArgument,
                 "client IPC requires a valid path, session factory and client limit", 0U});
        }
        try {
            std::lock_guard<std::mutex> workers_lock(workers_mutex_);
            workers_.reserve(maximum_clients_);
        } catch (...) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal,
                 "failed to reserve bounded client worker ownership", 0U});
        }

        const auto listener = CreateListener();
        if (!listener) {
            ReleaseEndpointLease();
            return common::Result<void, api::VehicleError>::Failure(listener.error());
        }
        {
            std::lock_guard<std::mutex> listener_lock(listener_mutex_);
            listener_fd_ = listener.value();
        }
        stopping_.store(false);
        running_.store(true);
        try {
            accept_thread_ = std::thread([this] { AcceptLoop(); });
        } catch (...) {
            running_.store(false);
            stopping_.store(true);
            InterruptListener();
            FinalizeListener();
            CleanupOwnedEndpoint();
            ReleaseEndpointLease();
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kInternal, "failed to start the client accept thread", 0U});
        }
        return common::Result<void, api::VehicleError>::Success();
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        return running_.load() && !stopping_.load();
    }

    void Shutdown() noexcept override {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
        while (shutdown_in_progress_) {
            if (g_active_client_server_thread == this) {
                // The active shutdown owner may be waiting to join this
                // server-owned thread. Returning lets that owner make progress.
                return;
            }
            lifecycle_changed_.wait(lifecycle_lock);
        }
        shutdown_in_progress_ = true;
        lifecycle_lock.unlock();

        (void)running_.exchange(false);
        stopping_.store(true);
        InterruptListener();

        if (accept_thread_.joinable() && accept_thread_.get_id() != std::this_thread::get_id()) {
            accept_thread_.join();
        }
        FinalizeListener();

        std::vector<Worker> workers;
        {
            std::lock_guard<std::mutex> workers_lock(workers_mutex_);
            workers.swap(workers_);
        }
        for (const auto& worker : workers) {
            worker.connection->Interrupt();
        }
        const auto caller = std::this_thread::get_id();
        const auto deferred = std::remove_if(
            workers.begin(),
            workers.end(),
            [caller](Worker& worker) {
                if (worker.thread.joinable() && worker.thread.get_id() == caller) {
                    // A request callback may initiate shutdown. Preserve its
                    // joinable thread under server ownership so an external
                    // owner can reap it after the callback unwinds. Detaching
                    // here would allow the worker to outlive server state.
                    return false;
                }
                if (worker.thread.joinable()) {
                    worker.thread.join();
                }
                return true;
            });
        workers.erase(deferred, workers.end());
        if (!workers.empty()) {
            std::lock_guard<std::mutex> workers_lock(workers_mutex_);
            // workers_ was swapped out above. Swap the retained caller back
            // without allocating inside this noexcept shutdown path.
            workers_.swap(workers);
        }
        CleanupOwnedEndpoint();
        ReleaseEndpointLease();

        lifecycle_lock.lock();
        shutdown_in_progress_ = false;
        lifecycle_lock.unlock();
        lifecycle_changed_.notify_all();
    }

private:
    struct Worker final {
        std::shared_ptr<ClientConnection> connection;
        std::shared_ptr<std::atomic<bool>> finished;
        std::uint32_t user_id{0U};
        std::thread thread;
    };

    [[nodiscard]] static bool IsTrustedParentDirectory(const struct stat& status) noexcept {
        const bool parent_is_sticky = (status.st_mode & S_ISVTX) != 0;
        const bool parent_is_shared_writable =
            (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
        return S_ISDIR(status.st_mode) &&
               (status.st_uid == ::geteuid() || status.st_uid == 0U) &&
               (!parent_is_shared_writable || parent_is_sticky);
    }

    [[nodiscard]] bool ParentPathMatches() const noexcept {
        if (!parent_directory_) {
            return false;
        }
        struct stat descriptor_status {};
        struct stat path_status {};
        return ::fstat(parent_directory_.Get(), &descriptor_status) == 0 &&
               ::lstat(parent_path_.c_str(), &path_status) == 0 &&
               IsTrustedParentDirectory(descriptor_status) &&
               IsTrustedParentDirectory(path_status) &&
               SameInode(descriptor_status, path_status) &&
               descriptor_status.st_dev == parent_device_ &&
               descriptor_status.st_ino == parent_inode_;
    }

    [[nodiscard]] bool LeasePathMatches() const noexcept {
        if (!lease_file_ || !ParentPathMatches()) {
            return false;
        }
        struct stat descriptor_status {};
        struct stat path_status {};
        return ::fstat(lease_file_.Get(), &descriptor_status) == 0 &&
               ::fstatat(
                   parent_directory_.Get(),
                   lease_name_.c_str(),
                   &path_status,
                   AT_SYMLINK_NOFOLLOW) == 0 &&
               IsTrustedLease(descriptor_status) && IsTrustedLease(path_status) &&
               SameInode(descriptor_status, path_status) &&
               descriptor_status.st_dev == lease_device_ &&
               descriptor_status.st_ino == lease_inode_;
    }

    common::Result<void, api::VehicleError> AcquireEndpointLease() {
        OwnedFileDescriptor parent_directory{
            ::open(
                parent_path_.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (!parent_directory) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("open client IPC parent directory"));
        }
        struct stat parent_status {};
        struct stat parent_path_status {};
        if (::fstat(parent_directory.Get(), &parent_status) != 0 ||
            ::lstat(parent_path_.c_str(), &parent_path_status) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("inspect client IPC parent directory"));
        }
        if (!IsTrustedParentDirectory(parent_status) ||
            !IsTrustedParentDirectory(parent_path_status) ||
            !SameInode(parent_status, parent_path_status)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC parent directory is not a stable trusted sticky or owner-controlled directory",
                 0U});
        }

        OwnedFileDescriptor lease{
            ::openat(
                parent_directory.Get(),
                lease_name_.c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                kLeaseMode)};
        if (!lease) {
            if (errno == ELOOP) {
                return common::Result<void, api::VehicleError>::Failure(
                    {api::VehicleErrorCode::kPermissionDenied,
                     "client IPC lease path is a symbolic link", 0U});
            }
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("open client IPC lease"));
        }
        struct stat lease_status {};
        if (::fstat(lease.Get(), &lease_status) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("inspect client IPC lease"));
        }
        if (!IsTrustedLease(lease_status)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC lease must be a single-link owner-only regular file", 0U});
        }
        if (::flock(lease.Get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return common::Result<void, api::VehicleError>::Failure(
                    {api::VehicleErrorCode::kPermissionDenied,
                     "client IPC lease is held by another process", 0U});
            }
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("lock client IPC lease"));
        }
        struct stat linked_lease {};
        if (::fstatat(
                parent_directory.Get(),
                lease_name_.c_str(),
                &linked_lease,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !IsTrustedLease(linked_lease) || !SameInode(lease_status, linked_lease)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC lease path changed while the lock was acquired", 0U});
        }

        parent_device_ = parent_status.st_dev;
        parent_inode_ = parent_status.st_ino;
        lease_device_ = lease_status.st_dev;
        lease_inode_ = lease_status.st_ino;
        parent_directory_ = std::move(parent_directory);
        lease_file_ = std::move(lease);
        return common::Result<void, api::VehicleError>::Success();
    }

    void ReleaseEndpointLease() noexcept {
        if (lease_file_) {
            ::flock(lease_file_.Get(), LOCK_UN);
        }
        lease_file_.Reset();
        parent_directory_.Reset();
        parent_device_ = 0;
        parent_inode_ = 0;
        lease_device_ = 0;
        lease_inode_ = 0;
    }

    common::Result<void, api::VehicleError> RecoverStaleEndpointIfPresent() {
        struct stat existing {};
        if (::fstatat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                &existing,
                AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                return common::Result<void, api::VehicleError>::Success();
            }
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("inspect client IPC path"));
        }
        if (!S_ISSOCK(existing.st_mode)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC path exists and is not a UNIX socket", 0U});
        }
        if (!IsTrustedEndpoint(existing)) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC endpoint owner or mode is not trusted", 0U});
        }
        if (!LeasePathMatches()) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC lease changed before endpoint recovery", 0U});
        }

        const auto probe = ProbeEndpoint(socket_path_);
        if (probe == EndpointProbeResult::kActive) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC endpoint is active; refusing to unlink", 0U});
        }
        if (probe != EndpointProbeResult::kStale) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kTransportDown,
                 "client IPC endpoint could not be proven stale by a bounded nonblocking connect",
                 0U});
        }

        struct stat confirmed {};
        if (!LeasePathMatches() ||
            ::fstatat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                &confirmed,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !IsTrustedEndpoint(confirmed) || !SameInode(existing, confirmed) ||
            existing.st_uid != confirmed.st_uid || existing.st_gid != confirmed.st_gid ||
            existing.st_mode != confirmed.st_mode || existing.st_nlink != confirmed.st_nlink) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC endpoint changed during stale recovery", 0U});
        }
        if (::unlinkat(parent_directory_.Get(), endpoint_name_.c_str(), 0) != 0) {
            return common::Result<void, api::VehicleError>::Failure(
                PosixError("unlink proven-stale client IPC endpoint"));
        }
        return common::Result<void, api::VehicleError>::Success();
    }

    common::Result<int, api::VehicleError> CreateListener() {
        const auto acquired = AcquireEndpointLease();
        if (!acquired) {
            return common::Result<int, api::VehicleError>::Failure(acquired.error());
        }
        const auto recovered = RecoverStaleEndpointIfPresent();
        if (!recovered) {
            return common::Result<int, api::VehicleError>::Failure(recovered.error());
        }
        if (!LeasePathMatches()) {
            return common::Result<int, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC lease changed before listener creation", 0U});
        }

        OwnedFileDescriptor listener{::socket(AF_UNIX, SOCK_STREAM, 0)};
        if (!listener) {
            return common::Result<int, api::VehicleError>::Failure(PosixError("create client socket"));
        }
        const int descriptor_flags = ::fcntl(listener.Get(), F_GETFD);
        if (descriptor_flags < 0 ||
            ::fcntl(listener.Get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
            return common::Result<int, api::VehicleError>::Failure(
                PosixError("mark client socket close-on-exec"));
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1U);
        if (!ParentPathMatches() || !LeasePathMatches()) {
            return common::Result<int, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC parent or lease changed before bind", 0U});
        }
        if (::bind(
                listener.Get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
            return common::Result<int, api::VehicleError>::Failure(
                PosixError("bind client IPC socket"));
        }
        struct stat bound {};
        if (::fstatat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                &bound,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISSOCK(bound.st_mode) || bound.st_uid != ::geteuid()) {
            return common::Result<int, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC socket ownership could not be established after bind", 0U});
        }
        bound_device_ = bound.st_dev;
        bound_inode_ = bound.st_ino;
        bound_owner_ = bound.st_uid;
        owns_endpoint_ = true;
        if (!LeasePathMatches()) {
            CleanupOwnedEndpoint();
            return common::Result<int, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC lease changed before endpoint permission setup", 0U});
        }
        if (::fchmodat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                kEndpointMode,
                0) != 0) {
            const auto error = PosixError("configure client IPC endpoint permissions");
            CleanupOwnedEndpoint();
            return common::Result<int, api::VehicleError>::Failure(error);
        }
        if (::listen(listener.Get(), static_cast<int>(maximum_clients_)) != 0) {
            const auto error = PosixError("listen on client IPC endpoint");
            CleanupOwnedEndpoint();
            return common::Result<int, api::VehicleError>::Failure(error);
        }
        struct stat confirmed {};
        if (!LeasePathMatches() ||
            ::fstatat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                &confirmed,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !IsTrustedEndpoint(confirmed) || PermissionBits(confirmed.st_mode) != kEndpointMode ||
            confirmed.st_dev != bound_device_ || confirmed.st_ino != bound_inode_ ||
            confirmed.st_uid != bound_owner_) {
            CleanupOwnedEndpoint();
            return common::Result<int, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client IPC endpoint changed while listener ownership was established", 0U});
        }
        return common::Result<int, api::VehicleError>::Success(listener.Release());
    }

    void InterruptListener() noexcept {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
        }
    }

    void FinalizeListener() noexcept {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        if (listener_fd_ >= 0) {
            ::close(listener_fd_);
            listener_fd_ = -1;
        }
    }

    void CleanupOwnedEndpoint() noexcept {
        if (!owns_endpoint_) {
            return;
        }
        struct stat current {};
        if (LeasePathMatches() &&
            ::fstatat(
                parent_directory_.Get(),
                endpoint_name_.c_str(),
                &current,
                AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(current.st_mode) &&
            current.st_dev == bound_device_ && current.st_ino == bound_inode_ &&
            current.st_uid == bound_owner_) {
            ::unlinkat(parent_directory_.Get(), endpoint_name_.c_str(), 0);
        }
        owns_endpoint_ = false;
    }

    void AcceptLoop() noexcept {
        const ScopedClientServerThread server_thread_context(this);
        while (!stopping_.load()) {
            int listener = -1;
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                listener = listener_fd_;
            }
            if (listener < 0) {
                break;
            }
            OwnedFileDescriptor client{::accept(listener, nullptr, nullptr)};
            if (!client) {
                if (errno == EINTR) {
                    continue;
                }
                if (stopping_.load() || errno == EBADF || errno == EINVAL) {
                    break;
                }
                continue;
            }
            if (stopping_.load()) {
                ShutdownSocket(client);
                break;
            }

            ReapFinishedWorkers();
            if (ActiveWorkerCount() >= maximum_clients_ || !ConfigureClient(client.Get())) {
                ShutdownSocket(client);
                continue;
            }

            const auto peer = ReadPeerCredentials(client.Get());
            if (!peer) {
                ShutdownSocket(client);
                continue;
            }
            if (ActiveWorkerCountForUser(peer.value().user_id) >= maximum_clients_per_user_) {
                ShutdownSocket(client);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                if (workers_.size() >= workers_.capacity()) {
                    ShutdownSocket(client);
                    continue;
                }
            }
            try {
                auto connection =
                    std::make_shared<ClientConnection>(std::move(client));
                auto finished = std::make_shared<std::atomic<bool>>(false);
                std::thread worker = worker_thread_factory_(
                    [this, connection, finished, identity = peer.value()] {
                        const ScopedClientServerThread worker_thread_context(this);
                        ServeClient(connection, identity);
                        finished->store(true);
                    });
                if (!worker.joinable()) {
                    throw std::runtime_error("client worker factory returned no thread");
                }
                std::lock_guard<std::mutex> lock(workers_mutex_);
                workers_.push_back(
                    Worker{connection, finished, peer.value().user_id, std::move(worker)});
            } catch (...) {
                // Either the accepted descriptor remains in client or ClientConnection owns it.
            }
        }
        ReapFinishedWorkers();
    }

    [[nodiscard]] bool ConfigureClient(int client) const noexcept {
        const int descriptor_flags = ::fcntl(client, F_GETFD);
        return descriptor_flags >= 0 &&
               ::fcntl(client, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
    }

    common::Result<api::PeerCredentials, api::VehicleError> ReadPeerCredentials(int client) const {
#ifdef SO_PEERCRED
        struct KernelPeerCredentials final {
            pid_t process_id;
            uid_t user_id;
            gid_t group_id;
        } credentials{};
        socklen_t length = sizeof(credentials);
        if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
            return common::Result<api::PeerCredentials, api::VehicleError>::Failure(
                PosixError("read client peer credentials"));
        }
        if (length != sizeof(credentials) || credentials.process_id <= 0) {
            return common::Result<api::PeerCredentials, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kPermissionDenied,
                 "client peer credentials are incomplete", 0U});
        }
        return common::Result<api::PeerCredentials, api::VehicleError>::Success(
            {static_cast<std::int64_t>(credentials.process_id),
             static_cast<std::uint32_t>(credentials.user_id),
             static_cast<std::uint32_t>(credentials.group_id)});
#else
        (void)client;
        return common::Result<api::PeerCredentials, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kNotSupported,
             "this POSIX target does not expose authenticated peer credentials", 0U});
#endif
    }

    void ServeClient(
        const std::shared_ptr<ClientConnection>& connection,
        const api::PeerCredentials& peer) noexcept {
        std::unique_ptr<api::ClientRequestSession> session;
        try {
            session = session_factory_();
            if (session) {
                ServeProtocol(connection, peer, *session);
            }
        } catch (...) {
            // A process boundary must contain application exceptions and close only this client.
        }
        connection->Interrupt();
        if (session) {
            session->Close();
        }
        connection->Finalize();
    }

    void ServeProtocol(
        const std::shared_ptr<ClientConnection>& connection,
        const api::PeerCredentials& peer,
        api::ClientRequestSession& session) {
        const auto first_frame = connection->Read(false);
        if (!first_frame) {
            return;
        }
        const auto decoded_hello = api::DecodeWireMessage(first_frame.value());
        if (!decoded_hello || !std::holds_alternative<api::Hello>(decoded_hello.value())) {
            const api::VehicleError error{
                api::VehicleErrorCode::kInvalidArgument,
                "the first client message must be a capability hello", 0U};
            (void)connection->Send(api::WireMessage{api::HelloAck{error, api::CurrentApiVersion()}});
            return;
        }

        const auto requested_version = std::get<api::Hello>(decoded_hello.value()).requested_version;
        const std::weak_ptr<ClientConnection> weak_connection = connection;
        const auto opened = session.Open(
            peer,
            requested_version,
            [weak_connection](api::WireMessage message) {
                const auto active = weak_connection.lock();
                return active && active->Send(std::move(message));
            });
        if (!opened) {
            (void)connection->Send(
                api::WireMessage{api::HelloAck{opened.error(), api::CurrentApiVersion()}});
            return;
        }
        if (!connection->Send(api::WireMessage{
                api::HelloAck{
                    {api::VehicleErrorCode::kOk, {}, 0U}, opened.value()}})) {
            return;
        }
        for (;;) {
            const auto frame = connection->Read(true);
            if (!frame) {
                return;
            }
            const auto decoded = api::DecodeWireMessage(frame.value());
            if (!decoded || !std::holds_alternative<api::TransportRequest>(decoded.value())) {
                return;
            }
            session.HandleRequest(std::get<api::TransportRequest>(decoded.value()));
        }
    }

    [[nodiscard]] std::size_t ActiveWorkerCount() const noexcept {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        std::size_t count = 0U;
        for (const auto& worker : workers_) {
            if (!worker.finished->load()) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t ActiveWorkerCountForUser(std::uint32_t user_id) const noexcept {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        std::size_t count = 0U;
        for (const auto& worker : workers_) {
            if (!worker.finished->load() && worker.user_id == user_id) {
                ++count;
            }
        }
        return count;
    }

    void ReapFinishedWorkers() noexcept {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        auto worker = workers_.begin();
        while (worker != workers_.end()) {
            if (!worker->finished->load()) {
                ++worker;
                continue;
            }
            if (worker->thread.joinable()) {
                if (worker->thread.get_id() == std::this_thread::get_id()) {
                    ++worker;
                    continue;
                }
                worker->thread.join();
            }
            worker = workers_.erase(worker);
        }
    }

    const std::string socket_path_;
    const std::string parent_path_;
    const std::string endpoint_name_;
    const std::string lease_name_;
    api::ClientRequestSessionFactory session_factory_;
    const std::size_t maximum_clients_;
    const std::size_t maximum_clients_per_user_;
    ClientWorkerThreadFactory worker_thread_factory_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    bool shutdown_in_progress_{false};
    mutable std::mutex listener_mutex_;
    mutable std::mutex workers_mutex_;
    std::vector<Worker> workers_;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{true};
    OwnedFileDescriptor parent_directory_;
    OwnedFileDescriptor lease_file_;
    int listener_fd_{-1};
    dev_t parent_device_{0};
    ino_t parent_inode_{0};
    dev_t lease_device_{0};
    ino_t lease_inode_{0};
    dev_t bound_device_{0};
    ino_t bound_inode_{0};
    uid_t bound_owner_{0};
    bool owns_endpoint_{false};
};

}  // namespace

std::unique_ptr<VehicleClientIpcServer> CreateHostPosixClientIpcServer(
    std::string socket_path,
    api::ClientRequestSessionFactory session_factory,
    std::size_t maximum_clients,
    std::size_t maximum_clients_per_user,
    ClientWorkerThreadFactory worker_thread_factory) {
    return std::make_unique<HostPosixClientIpcServer>(
        std::move(socket_path),
        std::move(session_factory),
        maximum_clients,
        maximum_clients_per_user,
        std::move(worker_thread_factory));
}

}  // namespace fw03::platform
