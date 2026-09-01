#include "fw03/common/task_executor.h"
#include "fw03/api/wire_codec.h"
#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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

}  // namespace
}  // namespace fw03::test
