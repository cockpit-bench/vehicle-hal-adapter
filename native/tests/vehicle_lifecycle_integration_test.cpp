#include "support/vehicle_stack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace fw03::test {
namespace {

using namespace std::chrono_literals;

class ManualQueueExecutor final : public common::TaskExecutor {
public:
    bool Post(std::function<void()> task) override {
        if (!task) {
            return false;
        }
        tasks_.push_back(std::move(task));
        return true;
    }

    void RunAll() {
        while (!tasks_.empty()) {
            auto task = std::move(tasks_.front());
            tasks_.erase(tasks_.begin());
            task();
        }
    }

private:
    std::vector<std::function<void()>> tasks_;
};

class BlockingCompletionExecutor final : public common::TaskExecutor {
public:
    bool Post(std::function<void()> task) override {
        std::unique_lock<std::mutex> lock(mutex_);
        completion_post_entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return release_completion_post_; });
        lock.unlock();
        task();
        return true;
    }

    bool PostControl(std::function<void()> task, bool) override {
        if (!task) {
            return false;
        }
        task();
        return true;
    }

    [[nodiscard]] bool WaitForCompletionPost() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(
            lock, std::chrono::seconds{1}, [this] { return completion_post_entered_; });
    }

    void ReleaseCompletionPost() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_completion_post_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool completion_post_entered_{false};
    bool release_completion_post_{false};
};

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

TEST_F(VehicleLifecycleIntegration, ImmediateSendFailureDoesNotAlsoInvokeCompletion) {
    stack_.transport->FailNextSend(
        {api::VehicleErrorCode::kTransportDown, "request write was rejected", 0U});
    std::size_t completion_count = 0U;
    const auto request = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        100ms,
        [&completion_count](middleware::ValueResult) { ++completion_count; },
        false);

    ASSERT_FALSE(request);
    EXPECT_EQ(request.error().code, api::VehicleErrorCode::kTransportDown);
    EXPECT_EQ(completion_count, 0U);
}

TEST_F(VehicleLifecycleIntegration, WrongPropertyResponseFailsOnceAndDisconnectsPeer) {
    std::size_t completion_count = 0U;
    std::optional<middleware::ValueResult> completion;
    const auto request = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&completion_count, &completion](middleware::ValueResult result) {
            ++completion_count;
            completion.emplace(std::move(result));
        },
        false);
    ASSERT_TRUE(request) << request.error().detail;

    stack_.transport->EmitResponse(
        {request.value(),
         {api::VehicleErrorCode::kOk, {}, request.value()},
         IntValue(kCabinTemperatureKey, 22, 1000)});

    ASSERT_TRUE(completion.has_value());
    ASSERT_FALSE(*completion);
    EXPECT_EQ(completion->error().code, api::VehicleErrorCode::kInternal);
    EXPECT_EQ(completion_count, 1U);
    EXPECT_EQ(stack_.transport->ShutdownCalls(), 1U);
    EXPECT_FALSE(stack_.service.IsConnected());

    stack_.transport->EmitResponse(
        {request.value(),
         {api::VehicleErrorCode::kOk, {}, request.value()},
         IntValue(kVehicleSpeedKey, 90, 1100)});
    EXPECT_EQ(completion_count, 1U);
}

TEST_F(VehicleLifecycleIntegration, ClosingOneSessionCancelsOnlyItsPendingRequests) {
    const auto second = stack_.OpenSession("second-owner");
    ASSERT_TRUE(second) << second.error().detail;

    std::optional<middleware::ValueResult> first_completion;
    std::optional<middleware::ValueResult> second_completion;
    const auto first = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&first_completion](middleware::ValueResult result) {
            first_completion.emplace(std::move(result));
        },
        false);
    const auto second_request = stack_.service.Get(
        second.value(),
        kVehicleSpeedKey,
        1s,
        [&second_completion](middleware::ValueResult result) {
            second_completion.emplace(std::move(result));
        },
        false);
    ASSERT_TRUE(first) << first.error().detail;
    ASSERT_TRUE(second_request) << second_request.error().detail;

    ASSERT_TRUE(stack_.service.CloseSession(session_id_));
    ASSERT_TRUE(first_completion.has_value());
    ASSERT_FALSE(*first_completion);
    EXPECT_EQ(first_completion->error().code, api::VehicleErrorCode::kCancelled);
    EXPECT_FALSE(second_completion.has_value());

    stack_.transport->EmitResponse(
        {first.value(),
         {api::VehicleErrorCode::kOk, {}, first.value()},
         IntValue(kVehicleSpeedKey, 1, 1000)});
    EXPECT_FALSE(second_completion.has_value());
    stack_.transport->EmitResponse(
        {second_request.value(),
         {api::VehicleErrorCode::kOk, {}, second_request.value()},
         IntValue(kVehicleSpeedKey, 2, 1100)});
    ASSERT_TRUE(second_completion.has_value());
    EXPECT_TRUE(*second_completion);
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

TEST_F(VehicleLifecycleIntegration, RejectedRateChangeRestoresPreviousCallbackAndRate) {
    std::size_t original_callback_count = 0U;
    std::size_t replacement_callback_count = 0U;
    constexpr api::SubscriberId kDirectSubscriber = 500U;
    ASSERT_TRUE(stack_.gateway.Subscribe(
        kDirectSubscriber,
        kVehicleSpeedKey,
        10.0F,
        [&original_callback_count](api::PropertyEvent) { ++original_callback_count; },
        100ms));

    stack_.transport->AcknowledgeNextControlWith(
        {api::VehicleErrorCode::kPermissionDenied, "rate change rejected", 0U});
    const auto changed = stack_.gateway.Subscribe(
        kDirectSubscriber,
        kVehicleSpeedKey,
        20.0F,
        [&replacement_callback_count](api::PropertyEvent) { ++replacement_callback_count; },
        100ms);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, api::VehicleErrorCode::kPermissionDenied);

    stack_.transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 55, 1000)});
    EXPECT_EQ(original_callback_count, 1U);
    EXPECT_EQ(replacement_callback_count, 0U);

    stack_.transport->ClearRequests();
    stack_.transport->EmitDeath();
    const auto reconnected = stack_.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    const auto replay = stack_.transport->Requests();
    ASSERT_EQ(replay.size(), 1U);
    EXPECT_EQ(replay.front().operation, api::TransportOperation::kSubscribe);
    EXPECT_FLOAT_EQ(replay.front().sample_rate_hz, 10.0F);
}

TEST_F(VehicleLifecycleIntegration, TimedOutControlAckRollsBackAndLateAckIsIgnored) {
    constexpr api::SubscriberId kDirectSubscriber = 501U;
    stack_.transport->SetAutoAcknowledgeControl(false);
    const auto timed_out = stack_.gateway.Subscribe(
        kDirectSubscriber,
        kVehicleSpeedKey,
        15.0F,
        [](api::PropertyEvent) {},
        10ms);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error().code, api::VehicleErrorCode::kTimeout);
    const auto first_requests = stack_.transport->Requests();
    ASSERT_EQ(first_requests.size(), 1U);

    stack_.transport->EmitResponse(
        {first_requests.front().request_id,
         {api::VehicleErrorCode::kOk, {}, first_requests.front().request_id},
         std::nullopt});
    EXPECT_FALSE(stack_.service.IsConnected());
    const auto reconnected = stack_.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    stack_.transport->ClearRequests();
    stack_.transport->SetAutoAcknowledgeControl(true);
    const auto retried = stack_.gateway.Subscribe(
        kDirectSubscriber,
        kVehicleSpeedKey,
        15.0F,
        [](api::PropertyEvent) {},
        100ms);
    ASSERT_TRUE(retried) << retried.error().detail;
    const auto retry_requests = stack_.transport->Requests();
    ASSERT_EQ(retry_requests.size(), 1U);
    EXPECT_EQ(retry_requests.front().operation, api::TransportOperation::kSubscribe);
    EXPECT_FLOAT_EQ(retry_requests.front().sample_rate_hz, 15.0F);
}

TEST_F(VehicleLifecycleIntegration, FailedReconnectReplayInvalidatesEpochAndNextAttemptReplaysAll) {
    constexpr api::SubscriberId kSpeedSubscriber = 601U;
    constexpr api::SubscriberId kTemperatureSubscriber = 602U;
    ASSERT_TRUE(stack_.gateway.Subscribe(
        kSpeedSubscriber,
        kVehicleSpeedKey,
        10.0F,
        [](api::PropertyEvent) {},
        100ms));
    ASSERT_TRUE(stack_.gateway.Subscribe(
        kTemperatureSubscriber,
        kCabinTemperatureKey,
        2.0F,
        [](api::PropertyEvent) {},
        100ms));
    stack_.transport->EmitDeath();
    stack_.transport->ClearRequests();

    stack_.transport->AcknowledgeNextControlWith(
        {api::VehicleErrorCode::kOk, {}, 0U});
    stack_.transport->AcknowledgeNextControlWith(
        {api::VehicleErrorCode::kPermissionDenied, "second replay rejected", 0U});
    const auto failed = stack_.service.Reconnect();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, api::VehicleErrorCode::kPermissionDenied);
    EXPECT_FALSE(stack_.service.IsConnected());
    EXPECT_EQ(stack_.transport->Requests().size(), 2U);

    stack_.transport->ClearRequests();
    const auto retried = stack_.service.Reconnect();
    ASSERT_TRUE(retried) << retried.error().detail;
    const auto replayed = stack_.transport->Requests();
    ASSERT_EQ(replayed.size(), 2U);
    EXPECT_EQ(replayed[0].operation, api::TransportOperation::kSubscribe);
    EXPECT_EQ(replayed[1].operation, api::TransportOperation::kSubscribe);
    EXPECT_EQ(replayed[0].key, kVehicleSpeedKey);
    EXPECT_EQ(replayed[1].key, kCabinTemperatureKey);
}

TEST_F(VehicleLifecycleIntegration, ClaimedControlCompletionWaitsForItsOwnerWithoutFallbackTimeout) {
    std::mutex slow_mutex;
    std::condition_variable slow_available;
    bool slow_completion_entered = false;
    bool allow_slow_completion = false;
    const auto pending_get = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&slow_mutex, &slow_available, &slow_completion_entered, &allow_slow_completion](
            middleware::ValueResult) {
            std::unique_lock<std::mutex> lock(slow_mutex);
            slow_completion_entered = true;
            slow_available.notify_all();
            slow_available.wait(lock, [&allow_slow_completion] { return allow_slow_completion; });
        },
        false);
    ASSERT_TRUE(pending_get) << pending_get.error().detail;

    stack_.transport->SetAutoAcknowledgeControl(false);
    std::optional<common::Result<void, api::VehicleError>> subscribe_result;
    std::atomic<bool> subscribe_finished{false};
    std::thread subscriber([this, &subscribe_result, &subscribe_finished] {
        subscribe_result.emplace(
            stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 10.0F, 20ms));
        subscribe_finished.store(true);
    });
    const auto request_deadline = std::chrono::steady_clock::now() + 500ms;
    while (stack_.transport->Requests().size() < 2U &&
           std::chrono::steady_clock::now() < request_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(stack_.transport->Requests().size(), 2U);

    std::thread death([this] {
        stack_.transport->EmitDeath(
            {api::VehicleErrorCode::kTransportDown, "peer disconnected", 0U});
    });
    {
        std::unique_lock<std::mutex> lock(slow_mutex);
        ASSERT_TRUE(slow_available.wait_for(
            lock,
            500ms,
            [&slow_completion_entered] { return slow_completion_entered; }));
    }
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(subscribe_finished.load());
    {
        std::lock_guard<std::mutex> lock(slow_mutex);
        allow_slow_completion = true;
    }
    slow_available.notify_all();

    death.join();
    subscriber.join();
    ASSERT_TRUE(subscribe_result.has_value());
    ASSERT_FALSE(*subscribe_result);
    EXPECT_EQ(subscribe_result->error().code, api::VehicleErrorCode::kTransportDown);
}

TEST_F(VehicleLifecycleIntegration, ClosedSessionIsNotReplayedAfterUnsubscribeIoFailure) {
    ASSERT_TRUE(stack_.service.Subscribe(session_id_, kVehicleSpeedKey, 20.0F));
    stack_.transport->ClearRequests();
    stack_.transport->FailNextSend(
        {api::VehicleErrorCode::kTransportDown, "unsubscribe write failed", 0U});

    const auto closed = stack_.service.CloseSession(session_id_);
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, api::VehicleErrorCode::kTransportDown);
    EXPECT_TRUE(stack_.transport->Requests().empty());
    EXPECT_FALSE(stack_.service.IsConnected());
    EXPECT_TRUE(stack_.service.CloseSession(session_id_));

    stack_.transport->EmitDeath();
    const auto reconnected = stack_.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    EXPECT_TRUE(stack_.transport->Requests().empty());
}

TEST_F(VehicleLifecycleIntegration, MultiPropertyCloseUsesOneTotalAckBudgetAndDoesNotReplay) {
    const std::set<api::PropertyKey> keys{
        {0x21000001U, 0U},
        {0x21000002U, 0U},
        {0x21000003U, 0U},
        {0x21000004U, 0U}};
    const auto session = stack_.service.OpenSession(
        {"multi-property-client", keys, {}},
        api::CurrentApiVersion(),
        {[](api::PropertyEvent) {}, {}});
    ASSERT_TRUE(session) << session.error().detail;
    for (const auto& key : keys) {
        ASSERT_TRUE(stack_.service.Subscribe(session.value(), key, 2.0F, 100ms));
    }

    stack_.transport->ClearRequests();
    stack_.transport->SetAutoAcknowledgeControl(false);
    const auto started_at = std::chrono::steady_clock::now();
    const auto closed = stack_.service.CloseSession(session.value());
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, api::VehicleErrorCode::kTimeout);
    EXPECT_LT(elapsed, 600ms);
    EXPECT_EQ(stack_.transport->Requests().size(), 1U);
    EXPECT_FALSE(stack_.service.IsConnected());

    stack_.transport->SetAutoAcknowledgeControl(true);
    stack_.transport->ClearRequests();
    const auto reconnected = stack_.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    EXPECT_TRUE(stack_.transport->Requests().empty());
}

TEST_F(VehicleLifecycleIntegration, FreshCacheExpiresAndFallsThroughToHal) {
    stack_.transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 40, 0)});
    std::optional<middleware::ValueResult> completion;
    const auto cached = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&completion](middleware::ValueResult result) { completion.emplace(std::move(result)); },
        true);
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached.value(), 0U);
    ASSERT_TRUE(completion.has_value());
    ASSERT_TRUE(*completion);
    EXPECT_EQ(std::get<std::int32_t>(completion->value()->payload), 40);

    stack_.clock.Advance(251ms);
    completion.reset();
    stack_.transport->ClearRequests();
    const auto refreshed = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&completion](middleware::ValueResult result) { completion.emplace(std::move(result)); },
        true);
    ASSERT_TRUE(refreshed);
    EXPECT_NE(refreshed.value(), 0U);
    EXPECT_FALSE(completion.has_value());
    const auto requests = stack_.transport->Requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().operation, api::TransportOperation::kGet);
    stack_.transport->EmitResponse(
        {refreshed.value(),
         {api::VehicleErrorCode::kOk, {}, refreshed.value()},
         IntValue(kVehicleSpeedKey, 41, 251000000)});
    ASSERT_TRUE(completion.has_value());
    ASSERT_TRUE(*completion);
}

TEST(VehicleLifecycleStandalone, CacheRequiresSubscriptionOrExplicitPolicy) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(adapter, clock, {}, 2s);
    ASSERT_TRUE(gateway.Start());

    constexpr std::size_t kUniquePeerKeys = 1024U;
    for (std::size_t index = 0U; index < kUniquePeerKeys; ++index) {
        const api::PropertyKey key{
            static_cast<std::uint32_t>(0x20000000U + index),
            static_cast<std::uint32_t>(index)};
        transport->EmitEvent(
            {1U, IntValue(key, static_cast<std::int32_t>(index), 0)});
    }

    EXPECT_EQ(gateway.CacheEntryCount(), 0U);
    EXPECT_EQ(gateway.CacheByteCount(), 0U);
    EXPECT_EQ(gateway.CachePolicyBypassCount(), kUniquePeerKeys);
    EXPECT_EQ(gateway.CacheAdmissionDropCount(), 0U);
    gateway.Shutdown();
}

TEST(VehicleLifecycleStandalone, DisabledCachePolicyStillFansOutWithoutRetainingPayload) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 0ms}},
        2s);
    ASSERT_TRUE(gateway.Start());

    std::size_t callback_count = 0U;
    ASSERT_TRUE(gateway.Subscribe(
        1U,
        kVehicleSpeedKey,
        10.0F,
        [&callback_count](api::PropertyEvent) { ++callback_count; },
        100ms));
    transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 42, 0)});
    transport->EmitEvent(
        {2U,
         {kVehicleSpeedKey,
          0,
          api::PropertyStatus::kAvailable,
          std::vector<std::uint8_t>((1024U * 1024U) + 1U, 0x7fU)}});

    EXPECT_EQ(callback_count, 1U);
    EXPECT_EQ(gateway.CacheEntryCount(), 0U);
    EXPECT_EQ(gateway.CacheByteCount(), 0U);
    EXPECT_EQ(gateway.CachePolicyBypassCount(), 1U);

    bool completion_called = false;
    const auto read = gateway.Get(
        kVehicleSpeedKey,
        100ms,
        [&completion_called](middleware::ValueResult) { completion_called = true; },
        true);
    ASSERT_TRUE(read);
    EXPECT_NE(read.value(), 0U);
    EXPECT_FALSE(completion_called);
    gateway.Shutdown();
}

TEST(VehicleLifecycleStandalone, CacheEnforcesEntryByteTotalAndDeterministicLruLimits) {
    constexpr api::PropertyKey kFirstKey{0x20000101U, 0U};
    constexpr api::PropertyKey kSecondKey{0x20000102U, 0U};
    constexpr api::PropertyKey kThirdKey{0x20000103U, 0U};
    constexpr api::PropertyKey kOversizedKey{0x20000104U, 0U};
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kFirstKey.property_id, 1s},
         {kSecondKey.property_id, 1s},
         {kThirdKey.property_id, 1s},
         {kOversizedKey.property_id, 1s}},
        0ms,
        {2U, 300U, 500U});
    ASSERT_TRUE(gateway.Start());

    const auto byte_value = [](api::PropertyKey key, std::uint8_t fill, std::size_t bytes) {
        return api::VehiclePropertyValue{
            key,
            0,
            api::PropertyStatus::kAvailable,
            std::vector<std::uint8_t>(bytes, fill)};
    };
    transport->EmitEvent({1U, byte_value(kFirstKey, 0x11U, 100U)});
    transport->EmitEvent({1U, byte_value(kSecondKey, 0x22U, 100U)});

    bool first_hit = false;
    const auto touch_first = gateway.Get(
        kFirstKey,
        100ms,
        [&first_hit](middleware::ValueResult result) { first_hit = result.ok(); },
        true);
    ASSERT_TRUE(touch_first);
    EXPECT_EQ(touch_first.value(), 0U);
    EXPECT_TRUE(first_hit);

    transport->EmitEvent({1U, byte_value(kThirdKey, 0x33U, 100U)});
    transport->EmitEvent({1U, byte_value(kOversizedKey, 0x44U, 300U)});

    EXPECT_EQ(gateway.CacheEntryCount(), 2U);
    EXPECT_LE(gateway.CacheByteCount(), 500U);
    EXPECT_EQ(gateway.CacheEvictionCount(), 1U);
    EXPECT_EQ(gateway.CacheAdmissionDropCount(), 1U);

    bool second_completion_called = false;
    const auto evicted_read = gateway.Get(
        kSecondKey,
        100ms,
        [&second_completion_called](middleware::ValueResult) {
            second_completion_called = true;
        },
        true);
    ASSERT_TRUE(evicted_read);
    EXPECT_NE(evicted_read.value(), 0U);
    EXPECT_FALSE(second_completion_called);
    gateway.Shutdown();
}

TEST(VehicleLifecycleStandalone, DeathPublishesBeforeReconnectWhileCompletionPostIsBlocked) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    BlockingCompletionExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    ASSERT_TRUE(adapter.Start());

    std::mutex observation_mutex;
    std::vector<bool> observed_states;
    adapter.SetTransportStateCallback(
        [&observation_mutex, &observed_states](bool connected, api::VehicleError) {
            std::lock_guard<std::mutex> lock(observation_mutex);
            observed_states.push_back(connected);
        });
    const auto pending = adapter.Get(
        kVehicleSpeedKey, 1s, [](hal::RequestResult) {}, 1U);
    ASSERT_TRUE(pending);

    std::thread death([&transport] { transport->EmitDeath(); });
    if (!executor.WaitForCompletionPost()) {
        executor.ReleaseCompletionPost();
        death.join();
        FAIL() << "death path did not reach the deterministic completion barrier";
    }
    const auto reconnected = adapter.Reconnect();
    executor.ReleaseCompletionPost();
    death.join();

    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    {
        std::lock_guard<std::mutex> lock(observation_mutex);
        EXPECT_EQ(observed_states, (std::vector<bool>{false, true}));
    }
    EXPECT_TRUE(adapter.IsConnected());
    adapter.Shutdown();
}

TEST(VehicleLifecycleStandalone, ImmediatePostHandshakeDeathCannotCommitConnectedState) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    std::vector<bool> observed_states;
    adapter.SetTransportStateCallback(
        [&observed_states](bool connected, api::VehicleError) {
            observed_states.push_back(connected);
        });
    // The action is owned by the mock, so retaining the shared_ptr here would create a
    // mock -> action -> mock ownership cycle and hide expectation verification at teardown.
    auto* const transport_observer = transport.get();
    EXPECT_CALL(*transport, Connect(testing::_))
        .WillOnce([transport_observer](const api::ApiVersion& version) {
            transport_observer->EmitDeath(
                {api::VehicleErrorCode::kTransportDown,
                 "peer died before Connect returned",
                 0U});
            return common::Result<api::ApiVersion, api::VehicleError>::Success(version);
        });

    const auto started = adapter.Start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, api::VehicleErrorCode::kTransportDown);
    EXPECT_FALSE(adapter.IsConnected());
    EXPECT_EQ(observed_states, (std::vector<bool>{false}));
    adapter.Shutdown();
}

TEST(VehicleLifecycleStandalone, ReplayEventSurvivesReconnectAndSuppressesSameSequence) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 1s}});
    bool emit_during_replay = false;
    // The default action lives inside the mock. Keep only a non-owning observer whose lifetime is
    // bounded by this test's transport owner, avoiding a self-retaining shared_ptr cycle.
    auto* const transport_observer = transport.get();
    ON_CALL(*transport, Send(testing::_, testing::_))
        .WillByDefault(
            [transport_observer, &emit_during_replay](
                const api::TransportRequest& request,
                std::chrono::milliseconds) {
                if (request.operation == api::TransportOperation::kSubscribe ||
                    request.operation == api::TransportOperation::kUnsubscribe) {
                    transport_observer->EmitResponse(
                        {request.request_id,
                         {api::VehicleErrorCode::kOk, {}, request.request_id},
                         std::nullopt});
                }
                if (request.operation == api::TransportOperation::kSubscribe &&
                    emit_during_replay) {
                    emit_during_replay = false;
                    transport_observer->EmitEvent(
                        {1U, IntValue(kVehicleSpeedKey, 42, 0)});
                }
                return common::Result<void, api::VehicleError>::Success();
            });
    ASSERT_TRUE(gateway.Start());

    std::size_t event_count = 0U;
    ASSERT_TRUE(gateway.Subscribe(
        1U,
        kVehicleSpeedKey,
        10.0F,
        [&event_count](api::PropertyEvent) { ++event_count; },
        100ms));
    transport->EmitDeath();
    emit_during_replay = true;
    const auto reconnected = gateway.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;

    EXPECT_EQ(event_count, 1U);
    EXPECT_EQ(gateway.CacheEntryCount(), 1U);
    transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 42, 0)});
    EXPECT_EQ(event_count, 1U);
    EXPECT_EQ(gateway.CacheEntryCount(), 1U);

    bool cache_hit = false;
    const auto cached = gateway.Get(
        kVehicleSpeedKey,
        100ms,
        [&cache_hit](middleware::ValueResult result) {
            cache_hit = result && result.value().has_value() &&
                        std::get<std::int32_t>(result.value()->payload) == 42;
        },
        true);
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached.value(), 0U);
    EXPECT_TRUE(cache_hit);
    gateway.Shutdown();
}

TEST(VehicleLifecycleStandalone, QueuedOldEpochEventCannotReachGatewayAfterReconnect) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    ManualQueueExecutor executor;
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 1s}});
    ASSERT_TRUE(gateway.Start());

    std::vector<std::uint64_t> observed_sequences;
    std::vector<bool> observed_states;
    gateway.SetStateCallback(
        [&observed_states](bool connected, api::VehicleError) {
            observed_states.push_back(connected);
        });
    ASSERT_TRUE(gateway.Subscribe(
        1U,
        kVehicleSpeedKey,
        10.0F,
        [&observed_sequences](api::PropertyEvent event) {
            observed_sequences.push_back(event.sequence);
        },
        100ms));

    transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 10, 0)});
    transport->EmitDeath();
    ASSERT_TRUE(gateway.Reconnect());
    transport->EmitEvent({2U, IntValue(kVehicleSpeedKey, 20, 0)});
    executor.RunAll();

    ASSERT_EQ(observed_sequences.size(), 1U);
    EXPECT_EQ(observed_sequences.front(), 2U);
    // The false control was published first but remained queued until after reconnect committed;
    // generation validation must reject that stale state instead of reporting true -> false.
    EXPECT_EQ(observed_states, (std::vector<bool>{true}));
    EXPECT_EQ(adapter.DroppedStaleEpochEventCount(), 1U);
    EXPECT_EQ(adapter.DroppedStateCallbackCount(), 1U);
    gateway.Shutdown();
}

TEST(VehicleLifecycleStandalone, FullEventQueueReservesOrderedDeathAndReconnectControl) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::SerialExecutor executor(2U);
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 1s}, {kCabinTemperatureProperty, 1s}});
    ASSERT_TRUE(gateway.Start());

    std::mutex observation_mutex;
    std::vector<std::uint64_t> observed_sequences;
    std::vector<bool> observed_states;
    gateway.SetStateCallback(
        [&observation_mutex, &observed_states](bool connected, api::VehicleError) {
            std::lock_guard<std::mutex> lock(observation_mutex);
            observed_states.push_back(connected);
        });
    const auto callback = [&observation_mutex, &observed_sequences](api::PropertyEvent event) {
        std::lock_guard<std::mutex> lock(observation_mutex);
        observed_sequences.push_back(event.sequence);
    };
    ASSERT_TRUE(gateway.Subscribe(1U, kVehicleSpeedKey, 10.0F, callback, 100ms));
    ASSERT_TRUE(gateway.Subscribe(1U, kCabinTemperatureKey, 5.0F, callback, 100ms));

    std::mutex blocker_mutex;
    std::condition_variable blocker_state;
    bool blocker_entered = false;
    bool release_blocker = false;
    ASSERT_TRUE(executor.Post([&] {
        std::unique_lock<std::mutex> lock(blocker_mutex);
        blocker_entered = true;
        blocker_state.notify_all();
        blocker_state.wait(lock, [&release_blocker] { return release_blocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(blocker_mutex);
        ASSERT_TRUE(blocker_state.wait_for(
            lock, 1s, [&blocker_entered] { return blocker_entered; }));
    }

    transport->EmitEvent({1U, IntValue(kVehicleSpeedKey, 10, 0)});
    transport->EmitEvent({1U, IntValue(kCabinTemperatureKey, 18, 0)});
    EXPECT_EQ(executor.PendingTaskCount(), 2U);
    transport->EmitDeath();
    ASSERT_TRUE(gateway.Reconnect());
    transport->EmitEvent({100U, IntValue(kVehicleSpeedKey, 100, 0)});

    {
        std::lock_guard<std::mutex> lock(blocker_mutex);
        release_blocker = true;
    }
    blocker_state.notify_all();
    executor.Drain();

    {
        std::lock_guard<std::mutex> lock(observation_mutex);
        EXPECT_EQ(observed_sequences, (std::vector<std::uint64_t>{100U}));
        EXPECT_EQ(observed_states, (std::vector<bool>{true}));
    }
    EXPECT_GE(executor.DiscardedCoalescingTaskCount(), 2U);
    EXPECT_EQ(executor.RejectedControlTaskCount(), 0U);
    EXPECT_EQ(adapter.DroppedEventCallbackCount(), 0U);
    EXPECT_EQ(adapter.DroppedStateCallbackCount(), 1U);
    gateway.Shutdown();
    executor.Shutdown();
}

TEST_F(VehicleLifecycleIntegration, OfflineCacheReportsStaleAndFutureEventsAreRejected) {
    stack_.transport->EmitEvent(
        {1U, IntValue(kVehicleSpeedKey, 90, 200000000)});
    stack_.transport->EmitEvent({2U, IntValue(kVehicleSpeedKey, 45, 0)});
    stack_.transport->EmitDeath();

    std::optional<middleware::ValueResult> completion;
    const auto result = stack_.service.Get(
        session_id_,
        kVehicleSpeedKey,
        1s,
        [&completion](middleware::ValueResult value) { completion.emplace(std::move(value)); },
        true);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0U);
    ASSERT_TRUE(completion.has_value());
    ASSERT_FALSE(*completion);
    EXPECT_EQ(completion->error().code, api::VehicleErrorCode::kStaleValue);
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

TEST(VehicleLifecycleStandalone, StartsOfflineAndReconnectsWhenVehiclePeerAppears) {
    VehicleStack stack;
    stack.transport->FailNextConnect(
        {api::VehicleErrorCode::kTransportDown, "vehicle peer is not ready", 0U});
    const auto started = stack.Start();
    ASSERT_TRUE(started) << started.error().detail;
    EXPECT_FALSE(stack.service.IsConnected());

    const auto session = stack.OpenSession("offline-client");
    ASSERT_TRUE(session) << session.error().detail;
    bool completion_called = false;
    const auto offline_get = stack.service.Get(
        session.value(),
        kVehicleSpeedKey,
        100ms,
        [&completion_called](middleware::ValueResult) { completion_called = true; },
        false);
    ASSERT_FALSE(offline_get);
    EXPECT_EQ(offline_get.error().code, api::VehicleErrorCode::kTransportDown);
    EXPECT_FALSE(completion_called);

    const auto reconnected = stack.service.Reconnect();
    ASSERT_TRUE(reconnected) << reconnected.error().detail;
    EXPECT_TRUE(stack.service.IsConnected());
    EXPECT_EQ(stack.transport->ConnectCalls(), 2U);
}

TEST(VehicleLifecycleStandalone, BurstEventsCoalesceByPropertyWhileConsumerIsBlocked) {
    auto transport = std::make_shared<testing::NiceMock<MockVehicleTransport>>();
    ManualClock clock;
    common::SerialExecutor executor(8U);
    hal::VehicleHalAdapter adapter(transport, clock, executor);
    middleware::VehiclePropertyGateway gateway(
        adapter,
        clock,
        {{kVehicleSpeedProperty, 250ms}});
    application::VehicleService service(gateway);
    ASSERT_TRUE(service.Start());

    std::mutex callback_mutex;
    std::condition_variable callback_available;
    bool first_callback_entered = false;
    bool release_first_callback = false;
    std::vector<std::uint64_t> observed_sequences;
    const auto session = service.OpenSession(
        {"burst-client", {kVehicleSpeedKey}, {}},
        api::CurrentApiVersion(),
        {[&](api::PropertyEvent event) {
             std::unique_lock<std::mutex> lock(callback_mutex);
             observed_sequences.push_back(event.sequence);
             if (!first_callback_entered) {
                 first_callback_entered = true;
                 callback_available.notify_all();
                 callback_available.wait(
                     lock,
                     [&release_first_callback] { return release_first_callback; });
             }
         },
         {}});
    ASSERT_TRUE(session) << session.error().detail;
    ASSERT_TRUE(service.Subscribe(session.value(), kVehicleSpeedKey, 20.0F, 100ms));

    transport->EmitEvent(
        {1U,
         {kVehicleSpeedKey,
          0,
          api::PropertyStatus::kAvailable,
          std::vector<std::uint8_t>(1024U * 1024U, 0x01U)}});
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        ASSERT_TRUE(callback_available.wait_for(
            lock,
            1s,
            [&first_callback_entered] { return first_callback_entered; }));
    }
    for (std::uint64_t sequence = 2U; sequence <= 16U; ++sequence) {
        transport->EmitEvent(
            {sequence,
             {kVehicleSpeedKey,
              0,
              api::PropertyStatus::kAvailable,
              std::vector<std::uint8_t>(1024U * 1024U, static_cast<std::uint8_t>(sequence))}});
    }
    EXPECT_EQ(executor.PendingTaskCount(), 1U);
    EXPECT_GE(executor.CoalescedTaskCount(), 14U);
    EXPECT_EQ(adapter.DroppedEventCallbackCount(), 0U);

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_first_callback = true;
    }
    callback_available.notify_all();
    executor.Drain();
    ASSERT_EQ(observed_sequences.size(), 2U);
    EXPECT_EQ(observed_sequences.front(), 1U);
    EXPECT_EQ(observed_sequences.back(), 16U);
    service.Shutdown();
    executor.Shutdown();
}

}  // namespace
}  // namespace fw03::test
