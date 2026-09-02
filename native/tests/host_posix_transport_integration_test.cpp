#include "fw03/platform/vehicle_transport.h"
#include "support/unix_vehicle_test_server.h"
#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::test {
namespace {

using namespace std::chrono_literals;

TEST(HostPosixTransportIntegration, PerformsCapabilityHandshakeAndPropertyRoundTrip) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());

    std::mutex mutex;
    std::condition_variable response_available;
    std::optional<api::TransportResponse> received;
    transport->SetCallbacks({
        [&mutex, &response_available, &received](api::TransportResponse response) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                received = std::move(response);
            }
            response_available.notify_all();
        },
        {},
        {},
    });

    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_TRUE(connected) << connected.error().detail;
    ASSERT_TRUE(transport->IsConnected());

    const api::TransportRequest request{1U, api::TransportOperation::kGet, kVehicleSpeedKey,
                                        0.0F, std::nullopt};
    const auto sent = transport->Send(request);
    ASSERT_TRUE(sent) << sent.error().detail;
    ASSERT_TRUE(server.WaitForRequests(1U, 2s)) << server.error();

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(response_available.wait_for(lock, 2s, [&received] { return received.has_value(); }));
    }
    ASSERT_TRUE(received->value.has_value());
    EXPECT_EQ(received->request_id, request.request_id);
    EXPECT_EQ(received->error.code, api::VehicleErrorCode::kOk);
    EXPECT_EQ(std::get<std::int32_t>(received->value->payload), 77);

    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ReportsPeerDeathExactlyOnce) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());

    std::mutex mutex;
    std::condition_variable death_available;
    std::size_t death_count = 0U;
    transport->SetCallbacks({
        {},
        {},
        [&mutex, &death_available, &death_count](api::VehicleError) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++death_count;
            }
            death_available.notify_all();
        },
    });
    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_TRUE(connected) << connected.error().detail;

    server.DisconnectClient();
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(death_available.wait_for(lock, 2s, [&death_count] { return death_count == 1U; }));
    }
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
    EXPECT_EQ(death_count, 1U);
}

TEST(HostPosixTransportIntegration, RejectsWorldWritableVehicleEndpoint) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    ASSERT_EQ(::chmod(server.path().c_str(), 0777), 0);

    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_NE(connected.error().detail.find("world-writable"), std::string::npos);
}

TEST(HostPosixTransportIntegration, RejectsUnexpectedVehiclePeerUid) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    const auto current_user = static_cast<std::uint32_t>(::geteuid());
    const auto other_user = current_user == std::numeric_limits<std::uint32_t>::max()
                                ? current_user - 1U
                                : current_user + 1U;
    auto transport = platform::CreateHostPosixVehicleTransport(
        server.path(),
        {other_user, static_cast<std::uint32_t>(::getegid()), true});
    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_NE(connected.error().detail.find("owner"), std::string::npos);
}

TEST(HostPosixTransportIntegration, RejectsUnexpectedVehiclePeerGid) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    const auto current_group = static_cast<std::uint32_t>(::getegid());
    const auto other_group = current_group == std::numeric_limits<std::uint32_t>::max()
                                 ? current_group - 1U
                                 : current_group + 1U;
    auto transport = platform::CreateHostPosixVehicleTransport(
        server.path(),
        {static_cast<std::uint32_t>(::geteuid()), other_group, true});
    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_NE(connected.error().detail.find("group"), std::string::npos);
}

TEST(HostPosixTransportIntegration, RejectsSymlinkInsteadOfAuthoritativeVehicleSocket) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    const auto alias = server.path() + ".alias";
    ASSERT_EQ(::symlink(server.path().c_str(), alias.c_str()), 0);
    auto transport = platform::CreateHostPosixVehicleTransport(alias);
    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_NE(connected.error().detail.find("not a UNIX socket"), std::string::npos);
    EXPECT_EQ(::unlink(alias.c_str()), 0);
}

TEST(HostPosixTransportIntegration, ConcurrentShutdownJoinsReaderExactlyOnce) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));

    std::vector<std::thread> shutdown_threads;
    for (std::size_t index = 0U; index < 4U; ++index) {
        shutdown_threads.emplace_back([transport] { transport->Shutdown(); });
    }
    for (auto& thread : shutdown_threads) {
        thread.join();
    }
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ReaderDeathCallbackOnlySignalsUntilExternalOwnerReaps) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    const std::weak_ptr<platform::VehicleTransport> weak_transport = transport;

    std::mutex mutex;
    std::condition_variable callback_finished;
    bool reader_shutdown_returned = false;
    transport->SetCallbacks({
        {},
        {},
        [&weak_transport, &mutex, &callback_finished, &reader_shutdown_returned](api::VehicleError) {
            if (const auto owner = weak_transport.lock()) {
                owner->Shutdown();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                reader_shutdown_returned = true;
            }
            callback_finished.notify_all();
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));

    server.DisconnectClient();
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(callback_finished.wait_for(
            lock,
            2s,
            [&reader_shutdown_returned] { return reader_shutdown_returned; }));
    }
    EXPECT_FALSE(transport->IsConnected());

    // The reader callback only signalled its own generation.  The external owner performs
    // the sole join/close, repeated shutdown remains idempotent, and destruction is safe.
    transport->Shutdown();
    transport->Shutdown();
    transport.reset();
}

TEST(HostPosixTransportIntegration, ExternalShutdownCanJoinAReaderThatAlsoRequestsShutdown) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    const std::weak_ptr<platform::VehicleTransport> weak_transport = transport;

    std::mutex callback_mutex;
    std::condition_variable callback_state_changed;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_returned = false;
    transport->SetCallbacks({
        [&weak_transport,
         &callback_mutex,
         &callback_state_changed,
         &callback_entered,
         &release_callback,
         &callback_returned](api::TransportResponse) {
            {
                std::unique_lock<std::mutex> lock(callback_mutex);
                callback_entered = true;
                callback_state_changed.notify_all();
                callback_state_changed.wait(lock, [&release_callback] { return release_callback; });
            }
            if (const auto owner = weak_transport.lock()) {
                owner->Shutdown();
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                callback_returned = true;
            }
            callback_state_changed.notify_all();
        },
        {},
        {},
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    const auto sent = transport->Send(
        {810U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt});
    ASSERT_TRUE(sent) << sent.error().detail;
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        ASSERT_TRUE(callback_state_changed.wait_for(
            lock,
            2s,
            [&callback_entered] { return callback_entered; }));
    }

    std::atomic<bool> external_shutdown_returned{false};
    std::thread external_owner([&transport, &external_shutdown_returned] {
        transport->Shutdown();
        external_shutdown_returned.store(true);
    });
    const auto signal_deadline = std::chrono::steady_clock::now() + 2s;
    while (transport->IsConnected() && std::chrono::steady_clock::now() < signal_deadline) {
        std::this_thread::yield();
    }
    const bool external_owner_signalled = !transport->IsConnected();
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_state_changed.notify_all();
    external_owner.join();

    EXPECT_TRUE(external_owner_signalled);
    EXPECT_TRUE(external_shutdown_returned.load());
    EXPECT_TRUE(callback_returned);
    EXPECT_FALSE(transport->IsConnected());
}

TEST(HostPosixTransportIntegration, ProtocolFailureRacesExternalShutdownWithoutSelfJoin) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    const std::weak_ptr<platform::VehicleTransport> weak_transport = transport;

    std::mutex callback_mutex;
    std::condition_variable callback_state_changed;
    bool death_callback_entered = false;
    bool release_death_callback = false;
    transport->SetCallbacks({
        {},
        {},
        [&weak_transport,
         &callback_mutex,
         &callback_state_changed,
         &death_callback_entered,
         &release_death_callback](api::VehicleError) {
            {
                std::unique_lock<std::mutex> lock(callback_mutex);
                death_callback_entered = true;
                callback_state_changed.notify_all();
                callback_state_changed.wait(
                    lock,
                    [&release_death_callback] { return release_death_callback; });
            }
            if (const auto owner = weak_transport.lock()) {
                owner->Shutdown();
            }
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(server.SendInvalidFrameLength(0U));
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        ASSERT_TRUE(callback_state_changed.wait_for(
            lock,
            2s,
            [&death_callback_entered] { return death_callback_entered; }));
    }

    std::atomic<bool> external_shutdown_started{false};
    std::thread external_owner([&transport, &external_shutdown_started] {
        external_shutdown_started.store(true);
        transport->Shutdown();
    });
    while (!external_shutdown_started.load()) {
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_death_callback = true;
    }
    callback_state_changed.notify_all();
    external_owner.join();

    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ThrowingResponseCallbackOnlyKillsItsTransport) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    std::mutex mutex;
    std::condition_variable death_available;
    std::optional<api::VehicleError> death_error;
    transport->SetCallbacks({
        [](api::TransportResponse) { throw std::runtime_error("response callback failure"); },
        {},
        [&mutex, &death_available, &death_error](api::VehicleError error) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                death_error.emplace(std::move(error));
            }
            death_available.notify_all();
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(transport->Send(
        {811U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(death_available.wait_for(
            lock,
            2s,
            [&death_error] { return death_error.has_value(); }));
    }
    EXPECT_EQ(death_error->code, api::VehicleErrorCode::kInternal);
    EXPECT_NE(death_error->detail.find("response callback"), std::string::npos);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ThrowingEventCallbackOnlyKillsItsTransport) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    std::mutex mutex;
    std::condition_variable death_available;
    std::optional<api::VehicleError> death_error;
    transport->SetCallbacks({
        {},
        [](api::PropertyEvent) { throw std::runtime_error("event callback failure"); },
        [&mutex, &death_available, &death_error](api::VehicleError error) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                death_error.emplace(std::move(error));
            }
            death_available.notify_all();
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(server.SendEvent(
        {812U,
         {kCabinTemperatureKey,
          1234567,
          api::PropertyStatus::kAvailable,
          std::int32_t{21}}}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(death_available.wait_for(
            lock,
            2s,
            [&death_error] { return death_error.has_value(); }));
    }
    EXPECT_EQ(death_error->code, api::VehicleErrorCode::kInternal);
    EXPECT_NE(death_error->detail.find("event callback"), std::string::npos);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ThrowingDeathCallbackDoesNotEscapeReaderThread) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    std::mutex mutex;
    std::condition_variable callback_entered;
    bool death_callback_entered = false;
    std::optional<api::VehicleError> observed_error;
    transport->SetCallbacks({
        {},
        {},
        [&mutex, &callback_entered, &death_callback_entered, &observed_error](api::VehicleError error) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                death_callback_entered = true;
                observed_error.emplace(std::move(error));
            }
            callback_entered.notify_all();
            throw std::runtime_error("death callback failure");
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    server.DisconnectClient();
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(callback_entered.wait_for(
            lock,
            2s,
            [&death_callback_entered] { return death_callback_entered; }));
    }
    EXPECT_EQ(observed_error->code, api::VehicleErrorCode::kTransportDown);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, SlowDrainCannotExtendASetBeyondItsAbsoluteDeadline) {
    UnixVehicleTestServer server(true);
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));

    api::VehiclePropertyValue value{
        kCabinTemperatureKey,
        1,
        api::PropertyStatus::kAvailable,
        std::vector<std::uint8_t>(1024U * 1024U, 0x5aU)};
    const api::TransportRequest request{
        700U,
        api::TransportOperation::kSet,
        kCabinTemperatureKey,
        0.0F,
        std::move(value)};
    const auto started_at = std::chrono::steady_clock::now();
    const auto sent = transport->Send(request, 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code, api::VehicleErrorCode::kTimeout);
    EXPECT_LT(elapsed, 500ms);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, PartialDownstreamFrameExpiresAndMarksTransportDead) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(server.path());
    std::mutex mutex;
    std::condition_variable death_available;
    std::optional<api::VehicleError> death_error;
    transport->SetCallbacks({
        [](api::TransportResponse) {},
        [](api::PropertyEvent) {},
        [&mutex, &death_available, &death_error](api::VehicleError error) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                death_error.emplace(std::move(error));
            }
            death_available.notify_all();
        },
    });
    ASSERT_TRUE(transport->Connect(api::CurrentApiVersion()));
    ASSERT_TRUE(server.SendPartialFrame(128U));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(death_available.wait_for(
            lock,
            2s,
            [&death_error] { return death_error.has_value(); }));
    }
    EXPECT_EQ(death_error->code, api::VehicleErrorCode::kTimeout);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ReaderThreadCreationFailureRollsBackConnection) {
    UnixVehicleTestServer server;
    ASSERT_TRUE(server.Start()) << server.error();
    auto transport = platform::CreateHostPosixVehicleTransport(
        server.path(),
        {},
        [](std::function<void()>) -> std::thread {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again));
        });

    const auto connected = transport->Connect(api::CurrentApiVersion());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code, api::VehicleErrorCode::kInternal);
    EXPECT_NE(connected.error().detail.find("reader thread"), std::string::npos);
    EXPECT_FALSE(transport->IsConnected());
    transport->Shutdown();
}

TEST(HostPosixTransportIntegration, ServiceStartsOfflineAcrossSilentHandshakeThenReconnects) {
    UnixVehicleTestServer silent_peer(false, true);
    ASSERT_TRUE(silent_peer.Start()) << silent_peer.error();
    auto transport = platform::CreateHostPosixVehicleTransport(silent_peer.path());
    common::SteadyClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 250ms}});
    application::VehicleService service(gateway);

    const auto started = service.Start();
    ASSERT_TRUE(started) << started.error().detail;
    EXPECT_FALSE(service.IsConnected());
    const auto session = service.OpenSession(
        {"offline-handshake-client", {kVehicleSpeedKey}, {}},
        api::CurrentApiVersion(),
        {[](api::PropertyEvent) {}, {}});
    ASSERT_TRUE(session) << session.error().detail;

    const auto peer_path = silent_peer.path();
    silent_peer.Stop();
    UnixVehicleTestServer live_peer(false, false, peer_path);
    ASSERT_TRUE(live_peer.Start()) << live_peer.error();
    const auto reconnected = service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    EXPECT_TRUE(service.IsConnected());
    service.Shutdown();
}

}  // namespace
}  // namespace fw03::test
