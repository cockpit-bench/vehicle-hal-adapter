#include "support/vehicle_stack.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace fw03::test {
namespace {

using namespace std::chrono_literals;

class VehicleServiceIntegration : public testing::Test {
protected:
    void SetUp() override {
        const auto started = stack_.Start();
        ASSERT_TRUE(started) << started.error().detail;
        const auto session = stack_.OpenSession("primary-client");
        ASSERT_TRUE(session) << session.error().detail;
        session_id_ = session.value();
    }

    VehicleStack stack_;
    api::SessionId session_id_{0U};
};

TEST_F(VehicleServiceIntegration, GetRoundTripTraversesServiceGatewayHalAndTransport) {
    EXPECT_CALL(*stack_.transport, Send(testing::_, testing::_)).Times(1);
    std::optional<middleware::ValueResult> completion;
    const auto request = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        250ms,
        [&completion](middleware::ValueResult result) { completion.emplace(std::move(result)); },
        false);
    ASSERT_TRUE(request) << request.error().detail;

    const auto sent = stack_.transport->Requests();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent.front().operation, api::TransportOperation::kGet);
    EXPECT_EQ(sent.front().key, kVehicleSpeedKey);

    auto value = IntValue(kVehicleSpeedKey, 88, 1000);
    stack_.transport->EmitResponse(
        {request.value(), {api::VehicleErrorCode::kOk, {}, request.value()}, value});

    ASSERT_TRUE(completion.has_value());
    ASSERT_TRUE(*completion) << completion->error().detail;
    ASSERT_TRUE(completion->value().has_value());
    EXPECT_EQ(std::get<std::int32_t>(completion->value()->payload), 88);
}

TEST_F(VehicleServiceIntegration, SetRoundTripPreservesTypedValueAndCompletion) {
    EXPECT_CALL(*stack_.transport, Send(testing::_, testing::_)).Times(1);
    std::optional<middleware::ValueResult> completion;
    const auto value = IntValue(kCabinTemperatureKey, 23, 2000);
    const auto request = stack_.service.Set(
        session_id_,
        value,
        250ms,
        [&completion](middleware::ValueResult result) { completion.emplace(std::move(result)); });
    ASSERT_TRUE(request) << request.error().detail;

    const auto sent = stack_.transport->Requests();
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_TRUE(sent.front().value.has_value());
    EXPECT_EQ(sent.front().operation, api::TransportOperation::kSet);
    EXPECT_EQ(sent.front().value.value(), value);

    stack_.transport->EmitResponse(
        {request.value(), {api::VehicleErrorCode::kOk, {}, request.value()}, std::nullopt});
    ASSERT_TRUE(completion.has_value());
    EXPECT_TRUE(*completion) << completion->error().detail;
    EXPECT_FALSE(completion->value().has_value());
}

TEST_F(VehicleServiceIntegration, MergesClientRatesAndUnsubscribesLastClient) {
    const auto second = stack_.OpenSession("secondary-client");
    ASSERT_TRUE(second) << second.error().detail;

    EXPECT_TRUE(stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 5.0F));
    EXPECT_TRUE(stack_.service.Subscribe(second.value(), kVehicleSpeedKey, 10.0F));
    EXPECT_TRUE(stack_.service.Unsubscribe(second.value(), kVehicleSpeedKey));
    EXPECT_TRUE(stack_.service.Unsubscribe(session_id_, kVehicleSpeedKey));

    const auto sent = stack_.transport->Requests();
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(sent[0].operation, api::TransportOperation::kSubscribe);
    EXPECT_FLOAT_EQ(sent[0].sample_rate_hz, 5.0F);
    EXPECT_EQ(sent[1].operation, api::TransportOperation::kSubscribe);
    EXPECT_FLOAT_EQ(sent[1].sample_rate_hz, 10.0F);
    EXPECT_EQ(sent[2].operation, api::TransportOperation::kSubscribe);
    EXPECT_FLOAT_EQ(sent[2].sample_rate_hz, 5.0F);
    EXPECT_EQ(sent[3].operation, api::TransportOperation::kUnsubscribe);
}

TEST_F(VehicleServiceIntegration, DropsDuplicateAndOutOfOrderEvents) {
    std::vector<api::PropertyEvent> events;
    ASSERT_TRUE(stack_.service.CloseSession(session_id_));
    const auto session = stack_.OpenSession(
        "event-client",
        {[&events](api::PropertyEvent event) { events.push_back(std::move(event)); }, {}});
    ASSERT_TRUE(session) << session.error().detail;
    session_id_ = session.value();
    ASSERT_TRUE(stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 10.0F));

    const auto first = IntValue(kVehicleSpeedKey, 30, 1000);
    stack_.transport->EmitEvent({10U, first});
    stack_.transport->EmitEvent({10U, first});
    stack_.transport->EmitEvent({9U, IntValue(kVehicleSpeedKey, 31, 1100)});
    stack_.transport->EmitEvent({11U, IntValue(kVehicleSpeedKey, 32, 1200)});

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].sequence, 10U);
    EXPECT_EQ(events[1].sequence, 11U);
}

TEST_F(VehicleServiceIntegration, RejectsIncompatibleClientMajorVersion) {
    auto incompatible = api::CurrentApiVersion();
    incompatible.major = 2U;
    incompatible.min_compatible_major = 2U;
    const auto session = stack_.service.OpenSession(
        {"future-client", {kVehicleSpeedKey}, {kVehicleSpeedKey}},
        incompatible,
        {[](api::PropertyEvent) {}, {}});
    ASSERT_FALSE(session);
    EXPECT_EQ(session.error().code, api::VehicleErrorCode::kIncompatibleVersion);
}

TEST_F(VehicleServiceIntegration, EnforcesPerSessionPropertyAccess) {
    const auto restricted = stack_.service.OpenSession(
        {"read-only-client", {kVehicleSpeedKey}, {}},
        api::CurrentApiVersion(),
        {[](api::PropertyEvent) {}, {}});
    ASSERT_TRUE(restricted) << restricted.error().detail;

    bool completion_called = false;
    const auto denied = stack_.service.Set(
        restricted.value(),
        IntValue(kVehicleSpeedKey, 10, 1),
        100ms,
        [&completion_called](middleware::ValueResult) { completion_called = true; });
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_FALSE(completion_called);
    EXPECT_TRUE(stack_.transport->Requests().empty());

    const auto wrong_area = stack_.service.Get(
        restricted.value(),
        {kVehicleSpeedProperty, 1U},
        100ms,
        [&completion_called](middleware::ValueResult) { completion_called = true; },
        false);
    ASSERT_FALSE(wrong_area);
    EXPECT_EQ(wrong_area.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_FALSE(completion_called);
}

TEST_F(VehicleServiceIntegration, CapsSubscriptionsPerSessionBeforeTransportMutation) {
    std::set<api::PropertyKey> readable;
    for (std::uint32_t index = 0U; index < 33U; ++index) {
        readable.insert({0x20000000U + index, 0U});
    }
    const auto bounded = stack_.service.OpenSession(
        {"bounded-subscriber", readable, {}},
        api::CurrentApiVersion(),
        {[](api::PropertyEvent) {}, {}});
    ASSERT_TRUE(bounded) << bounded.error().detail;

    std::size_t accepted = 0U;
    for (const auto& key : readable) {
        const auto subscribed =
            stack_.service.Subscribe(bounded.value(), key, 1.0F, 100ms);
        if (accepted < 32U) {
            ASSERT_TRUE(subscribed) << subscribed.error().detail;
            ++accepted;
        } else {
            ASSERT_FALSE(subscribed);
            EXPECT_EQ(subscribed.error().code, api::VehicleErrorCode::kInvalidArgument);
        }
    }
    EXPECT_EQ(accepted, 32U);
    EXPECT_EQ(stack_.transport->Requests().size(), 32U);
}

}  // namespace
}  // namespace fw03::test
