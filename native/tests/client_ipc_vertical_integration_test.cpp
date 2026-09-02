#include "fw03/application/vehicle_client_session.h"
#include "fw03/application/vehicle_service.h"
#include "fw03/common/clock.h"
#include "fw03/common/task_executor.h"
#include "fw03/hal/vehicle_hal_adapter.h"
#include "fw03/middleware/vehicle_property_gateway.h"
#include "fw03/platform/client_ipc_server.h"
#include "fw03/platform/vehicle_transport.h"
#include "support/unix_vehicle_test_server.h"
#include "support/vehicle_stack.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fw03::test {
namespace {

using namespace std::chrono_literals;

std::string UniqueClientSocketPath() {
    static std::atomic<std::uint64_t> counter{1U};
    return "/tmp/fw03-client-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter.fetch_add(1U)) + ".sock";
}

std::string ClientLeasePath(const std::string& socket_path) {
    return socket_path + ".lease";
}

class ScopedClientSocketPath final {
public:
    ScopedClientSocketPath() : path_(UniqueClientSocketPath()) {}

    ~ScopedClientSocketPath() {
        ::unlink(path_.c_str());
        ::unlink(ClientLeasePath(path_).c_str());
    }

    ScopedClientSocketPath(const ScopedClientSocketPath&) = delete;
    ScopedClientSocketPath& operator=(const ScopedClientSocketPath&) = delete;

    [[nodiscard]] const std::string& Get() const noexcept { return path_; }

private:
    std::string path_;
};

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedDescriptor() { Reset(); }

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    [[nodiscard]] int Get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

    void Reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_{-1};
};

int CreateBoundUnixSocket(const std::string& socket_path, bool make_listener) {
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    if (::bind(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0 ||
        ::chmod(socket_path.c_str(), 0660) != 0 ||
        (make_listener && ::listen(descriptor, 1) != 0)) {
        ::close(descriptor);
        ::unlink(socket_path.c_str());
        return -1;
    }
    return descriptor;
}

struct ObservedClientMessages final {
    std::mutex mutex;
    std::condition_variable available;
    std::map<api::RequestId, api::TransportResponse> responses;
    std::map<api::RequestId, std::size_t> response_counts;
    std::vector<api::PropertyEvent> events;
};

class EchoClientSession final : public api::ClientRequestSession {
public:
    explicit EchoClientSession(std::function<void()> on_request = {})
        : on_request_(std::move(on_request)) {}

    common::Result<api::ApiVersion, api::VehicleError> Open(
        const api::PeerCredentials&,
        const api::ApiVersion& requested_version,
        api::ClientMessageSink outbound) override {
        outbound_ = std::move(outbound);
        return api::NegotiateApiVersion(requested_version, api::CurrentApiVersion());
    }

    void HandleRequest(api::TransportRequest request) override {
        if (on_request_) {
            on_request_();
        }
        if (outbound_) {
            (void)outbound_(api::WireMessage{api::TransportResponse{
                request.request_id,
                {api::VehicleErrorCode::kOk, {}, request.request_id},
                std::nullopt}});
        }
    }

    void Close() noexcept override { outbound_ = {}; }

private:
    std::function<void()> on_request_;
    api::ClientMessageSink outbound_;
};

int ConnectRawClient(const std::string& socket_path) {
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(client);
        return -1;
    }
    return client;
}

TEST(ClientIpcVerticalIntegration, RoutesAuthenticatedRequestsResponsesAndEventsEndToEnd) {
    UnixVehicleTestServer downstream;
    ASSERT_TRUE(downstream.Start()) << downstream.error();

    auto downstream_transport =
        platform::CreateHostPosixVehicleTransport(downstream.path());
    common::SteadyClock clock;
    common::SerialExecutor executor;
    hal::VehicleHalAdapter hal_adapter(downstream_transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        hal_adapter,
        clock,
        {{kVehicleSpeedProperty, 250ms}, {kCabinTemperatureProperty, 2000ms}});
    application::VehicleService service(gateway);
    ASSERT_TRUE(service.Start());

    application::PropertyAllowlistPolicy policy(
        {static_cast<std::uint32_t>(::geteuid())},
        {static_cast<std::uint32_t>(::getegid())},
        {kVehicleSpeedKey, kCabinTemperatureKey},
        {kCabinTemperatureKey});
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [&service, &policy] {
            return std::make_unique<application::VehicleClientSession>(
                service,
                policy,
                500ms);
        },
        2U);
    ASSERT_TRUE(server->Start());
    ASSERT_TRUE(server->IsRunning());

    auto client = platform::CreateHostPosixVehicleTransport(client_path);
    ObservedClientMessages observed;
    client->SetCallbacks({
        [&observed](api::TransportResponse response) {
            {
                std::lock_guard<std::mutex> lock(observed.mutex);
                ++observed.response_counts[response.request_id];
                observed.responses[response.request_id] = std::move(response);
            }
            observed.available.notify_all();
        },
        [&observed](api::PropertyEvent event) {
            {
                std::lock_guard<std::mutex> lock(observed.mutex);
                observed.events.push_back(std::move(event));
            }
            observed.available.notify_all();
        },
        [](api::VehicleError) {},
    });
    ASSERT_TRUE(client->Connect(api::CurrentApiVersion()));

    const api::TransportRequest get_request{
        1U,
        api::TransportOperation::kGet,
        kVehicleSpeedKey,
        0.0F,
        std::nullopt};
    ASSERT_TRUE(client->Send(get_request));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return observed.responses.find(1U) != observed.responses.end(); }));
        const auto& response = observed.responses.at(1U);
        EXPECT_EQ(response.error.code, api::VehicleErrorCode::kOk);
        ASSERT_TRUE(response.value.has_value());
        EXPECT_EQ(response.value->key, kVehicleSpeedKey);
    }

    const api::TransportRequest set_request{
        2U,
        api::TransportOperation::kSet,
        kCabinTemperatureKey,
        0.0F,
        IntValue(kCabinTemperatureKey, 23, 2000)};
    ASSERT_TRUE(client->Send(set_request));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return observed.responses.find(2U) != observed.responses.end(); }));
        EXPECT_EQ(observed.responses.at(2U).error.code, api::VehicleErrorCode::kOk);
    }

    const api::TransportRequest subscribe_request{
        3U,
        api::TransportOperation::kSubscribe,
        kVehicleSpeedKey,
        10.0F,
        std::nullopt};
    ASSERT_TRUE(client->Send(subscribe_request));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return observed.responses.find(3U) != observed.responses.end(); }));
        EXPECT_EQ(observed.responses.at(3U).error.code, api::VehicleErrorCode::kOk);
    }
    ASSERT_TRUE(downstream.SendEvent(
        {7U, IntValue(kVehicleSpeedKey, 88, 3000)}));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return !observed.events.empty(); }));
        EXPECT_EQ(observed.events.front().sequence, 7U);
        EXPECT_EQ(observed.events.front().value.key, kVehicleSpeedKey);
    }

    const api::PropertyKey denied_key{0x12345678U, 0U};
    ASSERT_TRUE(client->Send(
        {4U, api::TransportOperation::kGet, denied_key, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return observed.responses.find(4U) != observed.responses.end(); }));
        EXPECT_EQ(
            observed.responses.at(4U).error.code,
            api::VehicleErrorCode::kPermissionDenied);
    }

    ASSERT_TRUE(downstream.WaitForRequests(3U, 2s));
    EXPECT_EQ(downstream.Requests().size(), 3U);

    ASSERT_TRUE(client->Send(
        {4U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(observed.mutex);
        ASSERT_TRUE(observed.available.wait_for(
            lock,
            2s,
            [&observed] { return observed.response_counts[4U] == 2U; }));
        EXPECT_EQ(
            observed.responses.at(4U).error.code,
            api::VehicleErrorCode::kInvalidArgument);
    }
    EXPECT_EQ(downstream.Requests().size(), 3U);

    client->Shutdown();
    ASSERT_TRUE(downstream.WaitForRequests(4U, 2s));
    const auto downstream_requests = downstream.Requests();
    ASSERT_EQ(downstream_requests.size(), 4U);
    EXPECT_EQ(
        downstream_requests.back().operation,
        api::TransportOperation::kUnsubscribe);
    server->Shutdown();
    EXPECT_FALSE(server->IsRunning());
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
    service.Shutdown();
    executor.Drain();
    executor.Shutdown();
}

TEST(ClientIpcVerticalIntegration, RefusesToUnlinkARegularFileAtTheConfiguredEndpoint) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    const int descriptor = ::open(client_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    ASSERT_GE(descriptor, 0);
    ASSERT_EQ(::close(descriptor), 0);

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::unique_ptr<api::ClientRequestSession>{}; });
    const auto started = server->Start();
    EXPECT_FALSE(started);
    struct stat endpoint {};
    EXPECT_EQ(::lstat(client_path.c_str(), &endpoint), 0);
    EXPECT_TRUE(S_ISREG(endpoint.st_mode));
    EXPECT_EQ(::unlink(client_path.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, RecoversAnOwnerTrustedCrashResidualSocket) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    {
        ScopedDescriptor crashed_lease{
            ::open(
                ClientLeasePath(client_path).c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600)};
        ASSERT_TRUE(crashed_lease);
        ASSERT_EQ(::flock(crashed_lease.Get(), LOCK_EX | LOCK_NB), 0);
    }
    {
        ScopedDescriptor crashed_listener{CreateBoundUnixSocket(client_path, false)};
        ASSERT_TRUE(crashed_listener);
    }
    struct stat residual {};
    ASSERT_EQ(::lstat(client_path.c_str(), &residual), 0);
    ASSERT_TRUE(S_ISSOCK(residual.st_mode));

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto started = server->Start();
    ASSERT_TRUE(started) << started.error().detail;
    EXPECT_TRUE(server->IsRunning());
    auto client = platform::CreateHostPosixVehicleTransport(client_path);
    ASSERT_TRUE(client->Connect(api::CurrentApiVersion()));
    client->Shutdown();
    server->Shutdown();
    struct stat removed {};
    EXPECT_NE(::lstat(client_path.c_str(), &removed), 0);
}

TEST(ClientIpcVerticalIntegration, PreservesARawLiveEndpointAfterBoundedProbe) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    ScopedDescriptor live_listener{CreateBoundUnixSocket(client_path, true)};
    ASSERT_TRUE(live_listener);
    struct stat before {};
    ASSERT_EQ(::lstat(client_path.c_str(), &before), 0);

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto started = server->Start();
    ASSERT_FALSE(started);
    EXPECT_NE(started.error().detail.find("endpoint is active"), std::string::npos);
    struct stat after {};
    ASSERT_EQ(::lstat(client_path.c_str(), &after), 0);
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);
    ScopedDescriptor probe{ConnectRawClient(client_path)};
    EXPECT_TRUE(probe);
}

TEST(ClientIpcVerticalIntegration, HeldLeasePreventsStaleEndpointCleanup) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    {
        ScopedDescriptor stale_listener{CreateBoundUnixSocket(client_path, false)};
        ASSERT_TRUE(stale_listener);
    }
    struct stat before {};
    ASSERT_EQ(::lstat(client_path.c_str(), &before), 0);
    ScopedDescriptor lease{
        ::open(
            ClientLeasePath(client_path).c_str(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600)};
    ASSERT_TRUE(lease);
    ASSERT_EQ(::flock(lease.Get(), LOCK_EX | LOCK_NB), 0);

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto blocked = server->Start();
    ASSERT_FALSE(blocked);
    EXPECT_NE(blocked.error().detail.find("lease is held"), std::string::npos);
    struct stat after {};
    ASSERT_EQ(::lstat(client_path.c_str(), &after), 0);
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);

    ASSERT_EQ(::flock(lease.Get(), LOCK_UN), 0);
    lease.Reset();
    const auto recovered = server->Start();
    ASSERT_TRUE(recovered) << recovered.error().detail;
    server->Shutdown();
}

TEST(ClientIpcVerticalIntegration, RefusesToFollowASymlinkAtTheConfiguredEndpoint) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    const auto target_path = client_path + ".target";
    ScopedDescriptor target{
        ::open(target_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
    ASSERT_TRUE(target);
    ASSERT_EQ(::symlink(target_path.c_str(), client_path.c_str()), 0);

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto started = server->Start();
    ASSERT_FALSE(started);
    struct stat preserved {};
    ASSERT_EQ(::lstat(client_path.c_str(), &preserved), 0);
    EXPECT_TRUE(S_ISLNK(preserved.st_mode));
    target.Reset();
    EXPECT_EQ(::unlink(target_path.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, RefusesToFollowASymlinkAtTheLeasePath) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    const auto lease_path = ClientLeasePath(client_path);
    const auto target_path = lease_path + ".target";
    ScopedDescriptor target{
        ::open(target_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
    ASSERT_TRUE(target);
    ASSERT_EQ(::symlink(target_path.c_str(), lease_path.c_str()), 0);

    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto started = server->Start();
    ASSERT_FALSE(started);
    EXPECT_NE(started.error().detail.find("symbolic link"), std::string::npos);
    struct stat preserved {};
    ASSERT_EQ(::lstat(lease_path.c_str(), &preserved), 0);
    EXPECT_TRUE(S_ISLNK(preserved.st_mode));
    target.Reset();
    EXPECT_EQ(::unlink(target_path.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, ShutdownPreservesAReplacementAtTheEndpointPath) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    const auto displaced_path = client_path + ".displaced";
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    ASSERT_TRUE(server->Start());
    ASSERT_EQ(::rename(client_path.c_str(), displaced_path.c_str()), 0);
    ScopedDescriptor replacement{
        ::open(client_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
    ASSERT_TRUE(replacement);
    struct stat before {};
    ASSERT_EQ(::lstat(client_path.c_str(), &before), 0);

    server->Shutdown();
    struct stat after {};
    ASSERT_EQ(::lstat(client_path.c_str(), &after), 0);
    EXPECT_TRUE(S_ISREG(after.st_mode));
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);
    replacement.Reset();
    EXPECT_EQ(::unlink(displaced_path.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, ShutdownPreservesEndpointAfterLeasePathReplacement) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    const auto lease_path = ClientLeasePath(client_path);
    const auto displaced_lease_path = lease_path + ".displaced";
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    ASSERT_TRUE(server->Start());
    struct stat endpoint_before {};
    ASSERT_EQ(::lstat(client_path.c_str(), &endpoint_before), 0);
    ASSERT_EQ(::rename(lease_path.c_str(), displaced_lease_path.c_str()), 0);
    ScopedDescriptor replacement_lease{
        ::open(
            lease_path.c_str(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600)};
    ASSERT_TRUE(replacement_lease);

    server->Shutdown();
    struct stat endpoint_after {};
    ASSERT_EQ(::lstat(client_path.c_str(), &endpoint_after), 0);
    EXPECT_TRUE(S_ISSOCK(endpoint_after.st_mode));
    EXPECT_EQ(endpoint_after.st_dev, endpoint_before.st_dev);
    EXPECT_EQ(endpoint_after.st_ino, endpoint_before.st_ino);
    replacement_lease.Reset();
    EXPECT_EQ(::unlink(displaced_lease_path.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, LeaseRejectsACompetingServerAndStartIsIdempotent) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto first = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::unique_ptr<api::ClientRequestSession>{}; });
    ASSERT_TRUE(first->Start());
    EXPECT_TRUE(first->Start());

    auto competing = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::unique_ptr<api::ClientRequestSession>{}; });
    const auto competing_start = competing->Start();
    EXPECT_FALSE(competing_start);
    EXPECT_NE(
        competing_start.error().detail.find("lease is held"),
        std::string::npos);

    first->Shutdown();
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
}

TEST(ClientIpcVerticalIntegration, RejectsAZeroClientLimitBeforeCreatingAnEndpoint) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::unique_ptr<api::ClientRequestSession>{}; },
        0U);
    EXPECT_FALSE(server->Start());
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
}

TEST(ClientIpcVerticalIntegration, RejectsGroupWritableNonStickyParentDirectory) {
    const auto parent = UniqueClientSocketPath() + ".dir";
    ASSERT_EQ(::mkdir(parent.c_str(), 0770), 0);
    ASSERT_EQ(::chmod(parent.c_str(), 0770), 0);
    const auto client_path = parent + "/gateway.sock";
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); });
    const auto started = server->Start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_NE(started.error().detail.find("trusted sticky"), std::string::npos);
    EXPECT_EQ(::rmdir(parent.c_str()), 0);
}

TEST(ClientIpcVerticalIntegration, DeniesPeersOutsideExplicitUidAndGidRules) {
    application::PropertyAllowlistPolicy policy(
        {1000U},
        {2000U},
        {kVehicleSpeedKey},
        {kCabinTemperatureKey});
    const auto allowed = policy.Authorize({42, 1000U, 2000U});
    ASSERT_TRUE(allowed) << allowed.error().detail;
    EXPECT_EQ(allowed.value().readable_properties.count(kVehicleSpeedKey), 1U);
    EXPECT_EQ(allowed.value().writable_properties.count(kCabinTemperatureKey), 1U);

    const auto wrong_user = policy.Authorize({42, 1001U, 2000U});
    ASSERT_FALSE(wrong_user);
    EXPECT_EQ(wrong_user.error().code, api::VehicleErrorCode::kPermissionDenied);
    const auto wrong_group = policy.Authorize({42, 1000U, 2001U});
    ASSERT_FALSE(wrong_group);
    EXPECT_EQ(wrong_group.error().code, api::VehicleErrorCode::kPermissionDenied);

    application::PropertyAllowlistPolicy deny_by_default({}, {}, {kVehicleSpeedKey}, {});
    EXPECT_FALSE(deny_by_default.Authorize({42, 1000U, 2000U}));
}

TEST(ClientIpcVerticalIntegration, PartialFrameDeadlineReleasesPerUserCapacity) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); },
        2U,
        1U);
    ASSERT_TRUE(server->Start());

    const int partial_client = ConnectRawClient(client_path);
    ASSERT_GE(partial_client, 0);
    const auto network_length = htonl(64U);
    ASSERT_EQ(
        ::send(partial_client, &network_length, sizeof(network_length), MSG_NOSIGNAL),
        static_cast<ssize_t>(sizeof(network_length)));
    const std::uint8_t one_byte = 0x01U;
    ASSERT_EQ(
        ::send(partial_client, &one_byte, sizeof(one_byte), MSG_NOSIGNAL),
        static_cast<ssize_t>(sizeof(one_byte)));

    auto quota_rejected = platform::CreateHostPosixVehicleTransport(client_path);
    EXPECT_FALSE(quota_rejected->Connect(api::CurrentApiVersion()));
    quota_rejected->Shutdown();

    std::this_thread::sleep_for(5200ms);
    auto recovered = platform::CreateHostPosixVehicleTransport(client_path);
    ASSERT_TRUE(recovered->Connect(api::CurrentApiVersion()));
    recovered->Shutdown();
    ::shutdown(partial_client, SHUT_RDWR);
    ::close(partial_client);
    server->Shutdown();
}

TEST(ClientIpcVerticalIntegration, IdleClientSurvivesBeyondFrameAssemblyDeadline) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); },
        1U,
        1U);
    ASSERT_TRUE(server->Start());

    auto client = platform::CreateHostPosixVehicleTransport(client_path);
    std::mutex mutex;
    std::condition_variable available;
    bool response_received = false;
    client->SetCallbacks({
        [&mutex, &available, &response_received](api::TransportResponse response) {
            if (response.request_id == 900U && response.error.code == api::VehicleErrorCode::kOk) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    response_received = true;
                }
                available.notify_all();
            }
        },
        [](api::PropertyEvent) {},
        [](api::VehicleError) {},
    });
    ASSERT_TRUE(client->Connect(api::CurrentApiVersion()));
    std::this_thread::sleep_for(5200ms);
    ASSERT_TRUE(client->Send(
        {900U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(available.wait_for(lock, 1s, [&response_received] { return response_received; }));
    }

    client->Shutdown();
    server->Shutdown();
}

TEST(ClientIpcVerticalIntegration, ConcurrentShutdownInterruptsPartialClientAndIsIdempotent) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); },
        2U,
        2U);
    ASSERT_TRUE(server->Start());
    const int partial_client = ConnectRawClient(client_path);
    ASSERT_GE(partial_client, 0);
    const std::uint16_t partial_header = 0U;
    ASSERT_EQ(
        ::send(partial_client, &partial_header, sizeof(partial_header), MSG_NOSIGNAL),
        static_cast<ssize_t>(sizeof(partial_header)));

    std::vector<std::thread> shutdown_threads;
    for (std::size_t index = 0U; index < 4U; ++index) {
        shutdown_threads.emplace_back([&server] { server->Shutdown(); });
    }
    for (auto& thread : shutdown_threads) {
        thread.join();
    }
    EXPECT_FALSE(server->IsRunning());
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
    ::close(partial_client);
}

TEST(ClientIpcVerticalIntegration, WorkerInitiatedShutdownDefersSelfJoinToTheExternalOwner) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    std::atomic<bool> worker_shutdown_completed{false};
    platform::VehicleClientIpcServer* server_view = nullptr;
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [&server_view, &worker_shutdown_completed] {
            return std::make_unique<EchoClientSession>(
                [&server_view, &worker_shutdown_completed] {
                    server_view->Shutdown();
                    worker_shutdown_completed.store(true);
                });
        },
        1U,
        1U);
    server_view = server.get();
    ASSERT_TRUE(server->Start());

    auto client = platform::CreateHostPosixVehicleTransport(client_path);
    client->SetCallbacks({
        [](api::TransportResponse) {},
        [](api::PropertyEvent) {},
        [](api::VehicleError) {},
    });
    ASSERT_TRUE(client->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(client->Send(
        {902U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    for (std::size_t attempt = 0U;
         attempt < 200U && !worker_shutdown_completed.load();
         ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(worker_shutdown_completed.load());

    // The worker kept its joinable thread under server ownership. A second
    // call from the unique owner must reap it without detach, deadlock or UAF.
    server->Shutdown();
    client->Shutdown();
    EXPECT_FALSE(server->IsRunning());
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
}

TEST(ClientIpcVerticalIntegration, ExternalShutdownDoesNotDeadlockWithWorkerShutdown) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    std::mutex barrier_mutex;
    std::condition_variable barrier_changed;
    bool worker_entered = false;
    bool allow_worker_shutdown = false;
    std::atomic<bool> worker_shutdown_completed{false};
    std::atomic<bool> external_shutdown_completed{false};
    platform::VehicleClientIpcServer* server_view = nullptr;
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [&] {
            return std::make_unique<EchoClientSession>([&] {
                {
                    std::unique_lock<std::mutex> lock(barrier_mutex);
                    worker_entered = true;
                    barrier_changed.notify_all();
                    barrier_changed.wait(lock, [&] { return allow_worker_shutdown; });
                }
                server_view->Shutdown();
                worker_shutdown_completed.store(true);
            });
        },
        1U,
        1U);
    server_view = server.get();
    ASSERT_TRUE(server->Start());

    auto client = platform::CreateHostPosixVehicleTransport(client_path);
    client->SetCallbacks({
        [](api::TransportResponse) {},
        [](api::PropertyEvent) {},
        [](api::VehicleError) {},
    });
    ASSERT_TRUE(client->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(client->Send(
        {903U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ASSERT_TRUE(barrier_changed.wait_for(lock, 1s, [&] { return worker_entered; }));
    }

    std::thread external_shutdown([&] {
        server->Shutdown();
        external_shutdown_completed.store(true);
    });
    for (std::size_t attempt = 0U; attempt < 200U && server->IsRunning(); ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_FALSE(server->IsRunning());
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        allow_worker_shutdown = true;
    }
    barrier_changed.notify_all();
    external_shutdown.join();

    EXPECT_TRUE(worker_shutdown_completed.load());
    EXPECT_TRUE(external_shutdown_completed.load());
    client->Shutdown();
    struct stat endpoint {};
    EXPECT_NE(::lstat(client_path.c_str(), &endpoint), 0);
}

TEST(ClientIpcVerticalIntegration, WorkerThreadCreationFailureClosesOnlyRejectedClient) {
    const ScopedClientSocketPath socket_artifacts;
    const auto& client_path = socket_artifacts.Get();
    auto attempts = std::make_shared<std::atomic<std::size_t>>(0U);
    auto server = platform::CreateHostPosixClientIpcServer(
        client_path,
        [] { return std::make_unique<EchoClientSession>(); },
        2U,
        2U,
        [attempts](std::function<void()> task) -> std::thread {
            if (attempts->fetch_add(1U) == 0U) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            return std::thread(std::move(task));
        });
    ASSERT_TRUE(server->Start());

    auto rejected = platform::CreateHostPosixVehicleTransport(client_path);
    EXPECT_FALSE(rejected->Connect(api::CurrentApiVersion()));
    rejected->Shutdown();

    auto recovered = platform::CreateHostPosixVehicleTransport(client_path);
    std::mutex response_mutex;
    std::condition_variable response_available;
    bool response_received = false;
    recovered->SetCallbacks({
        [&response_mutex, &response_available, &response_received](api::TransportResponse response) {
            if (response.request_id == 901U && response.error.code == api::VehicleErrorCode::kOk) {
                {
                    std::lock_guard<std::mutex> lock(response_mutex);
                    response_received = true;
                }
                response_available.notify_all();
            }
        },
        [](api::PropertyEvent) {},
        [](api::VehicleError) {},
    });
    ASSERT_TRUE(recovered->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(recovered->Send(
        {901U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(response_mutex);
        ASSERT_TRUE(response_available.wait_for(
            lock,
            1s,
            [&response_received] { return response_received; }));
    }
    EXPECT_TRUE(server->IsRunning());
    recovered->Shutdown();
    server->Shutdown();
}

}  // namespace
}  // namespace fw03::test
