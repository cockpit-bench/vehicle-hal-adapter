#include "fw03/common/task_executor.h"
#include "fw03/api/wire_codec.h"
#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::test {
namespace {

api::WireMessage RoundTrip(const api::WireMessage& message) {
    const auto encoded = api::EncodeWireMessage(message);
    EXPECT_TRUE(encoded) << encoded.error().detail;
    if (!encoded) {
        return api::Hello{};
    }
    const auto decoded = api::DecodeWireMessage(encoded.value());
    EXPECT_TRUE(decoded) << decoded.error().detail;
    if (!decoded) {
        return api::Hello{};
    }
    return decoded.value();
}

TEST(ContractCommonIntegration, RoundTripsEveryPropertyPayloadKind) {
    const std::vector<api::PropertyPayload> payloads{
        true,
        std::int32_t{-12345},
        std::int64_t{-9876543210LL},
        12.5F,
        -900.125,
        std::string("cabin-zone-left"),
        std::vector<std::uint8_t>{0U, 1U, 2U, 254U, 255U},
    };

    std::uint64_t sequence = 1U;
    for (const auto& payload : payloads) {
        const api::PropertyEvent original{
            sequence++,
            {kCabinTemperatureKey, -1234567, api::PropertyStatus::kAvailable, payload}};
        const auto decoded = RoundTrip(api::WireMessage{original});
        ASSERT_TRUE(std::holds_alternative<api::PropertyEvent>(decoded));
        EXPECT_EQ(std::get<api::PropertyEvent>(decoded).sequence, original.sequence);
        EXPECT_EQ(std::get<api::PropertyEvent>(decoded).value, original.value);
    }
}

TEST(ContractCommonIntegration, RoundTripsHandshakeRequestsResponsesAndEvents) {
    const auto hello = RoundTrip(api::WireMessage{api::Hello{api::CurrentApiVersion()}});
    ASSERT_TRUE(std::holds_alternative<api::Hello>(hello));
    EXPECT_EQ(std::get<api::Hello>(hello).requested_version, api::CurrentApiVersion());

    const api::HelloAck ack{
        {api::VehicleErrorCode::kPermissionDenied, "policy denied", 42U},
        api::CurrentApiVersion()};
    const auto decoded_ack = RoundTrip(api::WireMessage{ack});
    ASSERT_TRUE(std::holds_alternative<api::HelloAck>(decoded_ack));
    EXPECT_EQ(std::get<api::HelloAck>(decoded_ack).error.code,
              api::VehicleErrorCode::kPermissionDenied);

    const std::vector<api::TransportRequest> requests{
        {1U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt},
        {2U, api::TransportOperation::kSet, kVehicleSpeedKey, 0.0F,
         IntValue(kVehicleSpeedKey, 77, 100)},
        {3U, api::TransportOperation::kSubscribe, kVehicleSpeedKey, 12.5F, std::nullopt},
        {4U, api::TransportOperation::kUnsubscribe, kVehicleSpeedKey, 0.0F, std::nullopt},
    };
    for (const auto& request : requests) {
        const auto decoded = RoundTrip(api::WireMessage{request});
        ASSERT_TRUE(std::holds_alternative<api::TransportRequest>(decoded));
        const auto& actual = std::get<api::TransportRequest>(decoded);
        EXPECT_EQ(actual.request_id, request.request_id);
        EXPECT_EQ(actual.operation, request.operation);
        EXPECT_EQ(actual.key, request.key);
        EXPECT_EQ(actual.value.has_value(), request.value.has_value());
    }

    const api::TransportResponse response{
        9U,
        {api::VehicleErrorCode::kOk, {}, 9U},
        IntValue(kVehicleSpeedKey, 101, 999)};
    const auto decoded_response = RoundTrip(api::WireMessage{response});
    ASSERT_TRUE(std::holds_alternative<api::TransportResponse>(decoded_response));
    EXPECT_EQ(std::get<api::TransportResponse>(decoded_response).value, response.value);
}

TEST(ContractCommonIntegration, RejectsMalformedAndOversizedWireInput) {
    EXPECT_FALSE(api::DecodeWireMessage({}));

    const auto valid = api::EncodeWireMessage(api::WireMessage{api::Hello{api::CurrentApiVersion()}});
    ASSERT_TRUE(valid) << valid.error().detail;
    auto bad_magic = valid.value();
    bad_magic[0] ^= 0xffU;
    EXPECT_FALSE(api::DecodeWireMessage(bad_magic));

    auto future_framing = valid.value();
    future_framing[6] = 1U;
    EXPECT_FALSE(api::DecodeWireMessage(future_framing));

    auto trailing = valid.value();
    trailing.push_back(0U);
    EXPECT_FALSE(api::DecodeWireMessage(trailing));

    for (std::size_t size = 1U; size < valid.value().size(); ++size) {
        std::vector<std::uint8_t> truncated(valid.value().begin(),
                                            valid.value().begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_FALSE(api::DecodeWireMessage(truncated));
    }

    auto huge_value = IntValue(kVehicleSpeedKey, 1, 1);
    huge_value.payload = std::string((1024U * 1024U) + 1U, 'x');
    const api::TransportRequest huge_request{
        1U, api::TransportOperation::kSet, kVehicleSpeedKey, 0.0F, huge_value};
    EXPECT_FALSE(api::EncodeWireMessage(api::WireMessage{huge_request}));
}

TEST(ContractCommonIntegration, ValidatesRequestsVersionsAndStableErrors) {
    EXPECT_FALSE(api::ValidateRequest(
        {0U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    EXPECT_FALSE(api::ValidateRequest(
        {1U, api::TransportOperation::kGet, {0U, 0U}, 0.0F, std::nullopt}));
    EXPECT_FALSE(api::ValidateRequest(
        {2U, api::TransportOperation::kSet, kVehicleSpeedKey, 0.0F, std::nullopt}));
    EXPECT_FALSE(api::ValidateRequest(
        {3U, api::TransportOperation::kGet, kVehicleSpeedKey, 0.0F,
         IntValue(kVehicleSpeedKey, 1, 1)}));
    EXPECT_FALSE(api::ValidateRequest(
        {4U, api::TransportOperation::kSubscribe, kVehicleSpeedKey,
         std::numeric_limits<float>::infinity(), std::nullopt}));
    EXPECT_FALSE(api::ValidateRequest(
        {5U, api::TransportOperation::kSubscribe, kVehicleSpeedKey, 101.0F, std::nullopt}));

    auto future = api::CurrentApiVersion();
    future.major = 2U;
    future.min_compatible_major = 2U;
    EXPECT_FALSE(api::NegotiateApiVersion(future, api::CurrentApiVersion()));
    EXPECT_TRUE(api::NegotiateApiVersion(api::CurrentApiVersion(), api::CurrentApiVersion()));
    EXPECT_NE(api::CurrentApiVersion(), future);

    const std::vector<api::VehicleErrorCode> codes{
        api::VehicleErrorCode::kOk,
        api::VehicleErrorCode::kInvalidArgument,
        api::VehicleErrorCode::kNotSupported,
        api::VehicleErrorCode::kPermissionDenied,
        api::VehicleErrorCode::kTimeout,
        api::VehicleErrorCode::kTransportDown,
        api::VehicleErrorCode::kStaleValue,
        api::VehicleErrorCode::kCancelled,
        api::VehicleErrorCode::kInternal,
        api::VehicleErrorCode::kIncompatibleVersion,
    };
    for (const auto code : codes) {
        EXPECT_STRNE(api::ToString(code), "");
    }
    EXPECT_STREQ(api::ToString(static_cast<api::VehicleErrorCode>(999)), "INTERNAL");
}

TEST(ContractCommonIntegration, MapsUnknownPeerErrorToInternal) {
    const api::HelloAck ack{
        {api::VehicleErrorCode::kOk, "future detail", 7U}, api::CurrentApiVersion()};
    const auto encoded = api::EncodeWireMessage(api::WireMessage{ack});
    ASSERT_TRUE(encoded) << encoded.error().detail;
    auto bytes = encoded.value();
    ASSERT_GT(bytes.size(), 12U);
    bytes[9] = 99U;
    bytes[10] = 0U;
    bytes[11] = 0U;
    bytes[12] = 0U;
    const auto decoded = api::DecodeWireMessage(bytes);
    ASSERT_TRUE(decoded) << decoded.error().detail;
    ASSERT_TRUE(std::holds_alternative<api::HelloAck>(decoded.value()));
    EXPECT_EQ(std::get<api::HelloAck>(decoded.value()).error.code,
              api::VehicleErrorCode::kInternal);
}

TEST(ContractCommonIntegration, EmbedsThePinnedCanonicalProtobufDescriptor) {
    const auto descriptor = api::VehicleHalContractDescriptor();
    ASSERT_NE(descriptor.data, nullptr);
    ASSERT_GT(descriptor.size, 512U);
    const std::string bytes(
        reinterpret_cast<const char*>(descriptor.data),
        descriptor.size);
    EXPECT_NE(bytes.find("VehicleHalContract"), std::string::npos);
    EXPECT_NE(bytes.find("GetProperty"), std::string::npos);
    EXPECT_NE(bytes.find("StreamEvents"), std::string::npos);
}

TEST(ContractCommonIntegration, MatchesVersionedFixedWidthWireGoldenVectors) {
    const api::Hello hello{{1U, 2U, 3U, 1U}};
    const std::vector<std::uint8_t> hello_v1{
        0x33U, 0x30U, 0x57U, 0x46U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
        0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U, 0x01U, 0x00U};
    const auto encoded_hello = api::EncodeWireMessage(api::WireMessage{hello});
    ASSERT_TRUE(encoded_hello) << encoded_hello.error().detail;
    EXPECT_EQ(encoded_hello.value(), hello_v1);

    const api::TransportRequest request{
        0x0102030405060708ULL,
        api::TransportOperation::kGet,
        {0x11223344U, 0x55667788U},
        0.0F,
        std::nullopt};
    const std::vector<std::uint8_t> request_v1{
        0x33U, 0x30U, 0x57U, 0x46U, 0x01U, 0x00U, 0x00U, 0x00U, 0x03U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U, 0x01U,
        0x44U, 0x33U, 0x22U, 0x11U, 0x88U, 0x77U, 0x66U, 0x55U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    const auto encoded_request = api::EncodeWireMessage(api::WireMessage{request});
    ASSERT_TRUE(encoded_request) << encoded_request.error().detail;
    EXPECT_EQ(encoded_request.value(), request_v1);
    const auto decoded_request = api::DecodeWireMessage(request_v1);
    ASSERT_TRUE(decoded_request) << decoded_request.error().detail;
    ASSERT_TRUE(std::holds_alternative<api::TransportRequest>(decoded_request.value()));
    EXPECT_EQ(std::get<api::TransportRequest>(decoded_request.value()).request_id,
              request.request_id);

    const api::TransportResponse response{
        request.request_id,
        {api::VehicleErrorCode::kPermissionDenied, "no", request.request_id},
        std::nullopt};
    const std::vector<std::uint8_t> response_v1{
        0x33U, 0x30U, 0x57U, 0x46U, 0x01U, 0x00U, 0x00U, 0x00U, 0x04U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x03U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x02U, 0x00U, 0x00U, 0x00U, 0x6eU, 0x6fU, 0x00U};
    const auto encoded_response = api::EncodeWireMessage(api::WireMessage{response});
    ASSERT_TRUE(encoded_response) << encoded_response.error().detail;
    EXPECT_EQ(encoded_response.value(), response_v1);

    const api::PropertyEvent event{
        request.request_id,
        {{0x11223344U, 0x55667788U},
         static_cast<std::int64_t>(0x0102030405060708ULL),
         api::PropertyStatus::kAvailable,
         std::int32_t{-2}}};
    const std::vector<std::uint8_t> event_v1{
        0x33U, 0x30U, 0x57U, 0x46U, 0x01U, 0x00U, 0x00U, 0x00U, 0x05U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x44U, 0x33U, 0x22U, 0x11U, 0x88U, 0x77U, 0x66U, 0x55U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x00U, 0x01U, 0xfeU, 0xffU, 0xffU, 0xffU};
    const auto encoded_event = api::EncodeWireMessage(api::WireMessage{event});
    ASSERT_TRUE(encoded_event) << encoded_event.error().detail;
    EXPECT_EQ(encoded_event.value(), event_v1);
}

TEST(ContractCommonIntegration, SerialExecutorOrdersDrainsAndStopsTasks) {
    common::SerialExecutor executor;
    std::vector<std::uint32_t> order;
    std::mutex order_mutex;
    for (std::uint32_t value = 1U; value <= 32U; ++value) {
        ASSERT_TRUE(executor.Post([value, &order, &order_mutex] {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(value);
        }));
    }
    executor.Drain();
    ASSERT_EQ(order.size(), 32U);
    for (std::size_t index = 0U; index < order.size(); ++index) {
        EXPECT_EQ(order[index], static_cast<std::uint32_t>(index + 1U));
    }
    executor.Shutdown();
    executor.Shutdown();
    EXPECT_FALSE(executor.Post([] {}));
    EXPECT_FALSE(executor.Post({}));
}

TEST(ContractCommonIntegration, SerialExecutorWorkerShutdownDoesNotDeadlockExternalJoin) {
    common::SerialExecutor executor;
    std::mutex barrier_mutex;
    std::condition_variable barrier_changed;
    bool worker_entered = false;
    bool allow_worker_shutdown = false;
    std::atomic<bool> worker_shutdown_completed{false};
    std::atomic<bool> external_shutdown_completed{false};
    ASSERT_TRUE(executor.Post([&] {
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            worker_entered = true;
            barrier_changed.notify_all();
            barrier_changed.wait(lock, [&] { return allow_worker_shutdown; });
        }
        executor.Shutdown();
        worker_shutdown_completed.store(true);
    }));
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        ASSERT_TRUE(barrier_changed.wait_for(
            lock,
            std::chrono::seconds{1},
            [&] { return worker_entered; }));
    }

    std::thread external_shutdown([&] {
        executor.Shutdown();
        external_shutdown_completed.store(true);
    });
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        allow_worker_shutdown = true;
    }
    barrier_changed.notify_all();
    external_shutdown.join();

    EXPECT_TRUE(worker_shutdown_completed.load());
    EXPECT_TRUE(external_shutdown_completed.load());
    EXPECT_FALSE(executor.Post([] {}));
    executor.Shutdown();
}

TEST(ContractCommonIntegration, SerialExecutorBoundsAndCoalescesPendingWork) {
    common::SerialExecutor executor(4U);
    std::mutex gate_mutex;
    std::condition_variable gate_available;
    bool blocker_started = false;
    bool release_blocker = false;
    ASSERT_TRUE(executor.Post([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        blocker_started = true;
        gate_available.notify_all();
        gate_available.wait(lock, [&release_blocker] { return release_blocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_available.wait_for(
            lock,
            std::chrono::seconds{1},
            [&blocker_started] { return blocker_started; }));
    }

    std::atomic<std::uint32_t> latest_value{0U};
    std::atomic<std::size_t> coalesced_executions{0U};
    for (std::uint32_t value = 1U; value <= 100U; ++value) {
        ASSERT_TRUE(executor.PostCoalescing(7U, [value, &latest_value, &coalesced_executions] {
            latest_value.store(value);
            coalesced_executions.fetch_add(1U);
        }));
    }
    ASSERT_TRUE(executor.PostCoalescing(8U, [] {}));
    ASSERT_TRUE(executor.PostCoalescing(9U, [] {}));
    ASSERT_TRUE(executor.PostCoalescing(10U, [] {}));
    EXPECT_EQ(executor.PendingTaskCount(), 4U);
    EXPECT_GE(executor.CoalescedTaskCount(), 99U);
    EXPECT_FALSE(executor.PostCoalescing(11U, [] {}));
    EXPECT_EQ(executor.RejectedTaskCount(), 1U);

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_blocker = true;
    }
    gate_available.notify_all();
    executor.Drain();
    EXPECT_EQ(latest_value.load(), 100U);
    EXPECT_EQ(coalesced_executions.load(), 1U);
    executor.Shutdown();
}

TEST(ContractCommonIntegration, SerialExecutorControlDiscardsOnlyObsoleteEventGeneration) {
    common::SerialExecutor executor(3U);
    std::mutex gate_mutex;
    std::condition_variable gate_available;
    bool blocker_started = false;
    bool release_blocker = false;
    ASSERT_TRUE(executor.Post([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        blocker_started = true;
        gate_available.notify_all();
        gate_available.wait(lock, [&release_blocker] { return release_blocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_available.wait_for(
            lock,
            std::chrono::seconds{1},
            [&blocker_started] { return blocker_started; }));
    }

    std::vector<std::uint32_t> execution_order;
    ASSERT_TRUE(executor.PostCoalescing(
        10U, [&execution_order] { execution_order.push_back(1U); }));
    ASSERT_TRUE(executor.PostEventCoalescing(
        20U, 1U, [&execution_order] { execution_order.push_back(2U); }));
    ASSERT_TRUE(executor.PostEventCoalescing(
        30U, 2U, [&execution_order] { execution_order.push_back(3U); }));
    ASSERT_TRUE(executor.PostConnectionControl(
        1U, [&execution_order] { execution_order.push_back(4U); }));
    EXPECT_EQ(executor.PendingTaskCount(), 3U);
    EXPECT_EQ(executor.DiscardedCoalescingTaskCount(), 1U);
    EXPECT_EQ(executor.RejectedControlTaskCount(), 0U);

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_blocker = true;
    }
    gate_available.notify_all();
    executor.Drain();
    EXPECT_EQ(execution_order, (std::vector<std::uint32_t>{1U, 3U, 4U}));
    executor.Shutdown();
}

}  // namespace
}  // namespace fw03::test
