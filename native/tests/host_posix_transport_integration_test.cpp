#include "fw03/platform/vehicle_transport.h"
#include "support/unix_vehicle_test_server.h"
#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

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

}  // namespace
}  // namespace fw03::test
