#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace fw03::test {
namespace {

using namespace std::chrono_literals;

class VehicleLifecycleIntegration : public testing::Test {
protected:
    void SetUp() override {
        const auto started = stack_.Start();
        ASSERT_TRUE(started) << started.error().detail;
        const auto session = stack_.OpenSession(
            "lifecycle-client",
            {[this](api::PropertyEvent) { event_count_.fetch_add(1); },
             [this](bool connected, api::VehicleError error) {
                 connected_.store(connected);
                 last_state_error_.store(static_cast<std::int32_t>(error.code));
                 state_callback_count_.fetch_add(1);
             }});
        ASSERT_TRUE(session) << session.error().detail;
        session_id_ = session.value();
    }

    VehicleStack stack_;
    api::SessionId session_id_{0U};
    std::atomic<std::size_t> event_count_{0U};
    std::atomic<std::size_t> state_callback_count_{0U};
    std::atomic<bool> connected_{true};
    std::atomic<std::int32_t> last_state_error_{
        static_cast<std::int32_t>(api::VehicleErrorCode::kOk)};
};

TEST_F(VehicleLifecycleIntegration, TimeoutCompletesOnceAndIgnoresLateResponse) {
    std::size_t completion_count = 0U;
    std::optional<middleware::ValueResult> completion;
    const auto request = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        50ms,
        [&completion_count, &completion](middleware::ValueResult result) {
            ++completion_count;
            completion.emplace(std::move(result));
        },
        false);
    ASSERT_TRUE(request) << request.error().detail;

    stack_.clock.Advance(51ms);
    stack_.service.PollTimeouts();
    ASSERT_TRUE(completion.has_value());
    ASSERT_FALSE(*completion);
    EXPECT_EQ(completion->error().code, api::VehicleErrorCode::kTimeout);

    stack_.transport->EmitResponse(
        {request.value(),
         {api::VehicleErrorCode::kOk, {}, request.value()},
         IntValue(kVehicleSpeedKey, 100, 1000)});
    EXPECT_EQ(completion_count, 1U);
}

TEST_F(VehicleLifecycleIntegration, TransportDeathFailsPendingAndNotifiesSession) {
    std::optional<middleware::ValueResult> completion;
    const auto request = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&completion](middleware::ValueResult result) { completion.emplace(std::move(result)); },
        false);
    ASSERT_TRUE(request) << request.error().detail;

    stack_.transport->EmitDeath();
    ASSERT_TRUE(completion.has_value());
    ASSERT_FALSE(*completion);
    EXPECT_EQ(completion->error().code, api::VehicleErrorCode::kTransportDown);
    EXPECT_FALSE(connected_.load());
    EXPECT_EQ(state_callback_count_.load(), 1U);
    EXPECT_EQ(last_state_error_.load(),
              static_cast<std::int32_t>(api::VehicleErrorCode::kTransportDown));
}

TEST_F(VehicleLifecycleIntegration, ReconnectRestoresEffectiveSubscriptionWithoutDuplicateCallback) {
    ASSERT_TRUE(stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 20.0F));
    stack_.transport->ClearRequests();
    stack_.transport->EmitDeath();

    const auto reconnected = stack_.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    EXPECT_TRUE(connected_.load());
    ASSERT_EQ(stack_.transport->ConnectCalls(), 2U);

    const auto replay = stack_.transport->Requests();
    ASSERT_EQ(replay.size(), 1U);
    EXPECT_EQ(replay.front().operation, api::TransportOperation::kSubscribe);
    EXPECT_EQ(replay.front().key, kVehicleSpeedKey);
    EXPECT_FLOAT_EQ(replay.front().sample_rate_hz, 20.0F);

    const auto event = api::PropertyEvent{50U, IntValue(kVehicleSpeedKey, 45, 5000)};
    stack_.transport->EmitEvent(event);
    stack_.transport->EmitEvent(event);
    EXPECT_EQ(event_count_.load(), 1U);
}

TEST_F(VehicleLifecycleIntegration, ConcurrentEventDeliveryAndRepeatedShutdownAreSafe) {
    ASSERT_TRUE(stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 25.0F));
    std::atomic<bool> start{false};
    std::thread producer([this, &start] {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (std::uint64_t sequence = 1U; sequence <= 200U; ++sequence) {
            stack_.transport->EmitEvent(
                {sequence,
                 IntValue(kVehicleSpeedKey,
                          static_cast<std::int32_t>(sequence),
                          static_cast<std::int64_t>(sequence * 100U))});
        }
    });

    start.store(true);
    stack_.service.Shutdown();
    stack_.service.Shutdown();
    producer.join();

    const auto callbacks_after_shutdown = event_count_.load();
    stack_.transport->EmitEvent({999U, IntValue(kVehicleSpeedKey, 99, 99900)});
    EXPECT_EQ(event_count_.load(), callbacks_after_shutdown);
    EXPECT_FALSE(stack_.service.IsConnected());
}

}  // namespace
}  // namespace fw03::test
