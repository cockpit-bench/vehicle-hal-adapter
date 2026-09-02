#pragma once

#include "fw03/common/result.h"
#include "fw03/common/clock.h"
#include "fw03/api/vehicle_contract.h"
#include "fw03/hal/vehicle_hal_adapter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace fw03::middleware {

using ValueResult = hal::RequestResult;
using ValueCompletion = hal::RequestCompletion;
using ClientEventCallback = std::function<void(api::PropertyEvent)>;
using GatewayStateCallback = std::function<void(bool, api::VehicleError)>;

inline constexpr std::size_t kGatewayCacheHardMaximumEntries = 256U;
inline constexpr std::size_t kGatewayCacheHardMaximumEntryBytes =
    (1024U * 1024U) + 256U;
inline constexpr std::size_t kGatewayCacheHardMaximumTotalBytes = 8U * 1024U * 1024U;

struct GatewayCacheLimits final {
    std::size_t maximum_entries{kGatewayCacheHardMaximumEntries};
    std::size_t maximum_entry_bytes{kGatewayCacheHardMaximumEntryBytes};
    std::size_t maximum_total_bytes{kGatewayCacheHardMaximumTotalBytes};
};

class VehiclePropertyGateway final {
public:
    VehiclePropertyGateway(
        hal::VehicleHalAdapter& hal_adapter,
        common::Clock& clock,
        std::map<std::uint32_t, std::chrono::milliseconds> cache_max_age_by_property,
        std::chrono::milliseconds default_cache_max_age = std::chrono::milliseconds::zero(),
        GatewayCacheLimits cache_limits = {});
    ~VehiclePropertyGateway();

    VehiclePropertyGateway(const VehiclePropertyGateway&) = delete;
    VehiclePropertyGateway& operator=(const VehiclePropertyGateway&) = delete;

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Start(
        const api::ApiVersion& requested_version = api::CurrentApiVersion());

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Get(
        api::PropertyKey key,
        std::chrono::milliseconds timeout,
        ValueCompletion completion,
        bool prefer_cache = true,
        api::SessionId owner_id = 0U);

    [[nodiscard]] common::Result<api::RequestId, api::VehicleError> Set(
        api::VehiclePropertyValue value,
        std::chrono::milliseconds timeout,
        ValueCompletion completion,
        api::SessionId owner_id = 0U);

    [[nodiscard]] common::Result<void, api::VehicleError> Subscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        float sample_rate_hz,
        ClientEventCallback callback,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] common::Result<void, api::VehicleError> Unsubscribe(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    [[nodiscard]] common::Result<void, api::VehicleError> ReleaseSubscriber(
        api::SubscriberId subscriber_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    void CancelOwner(api::SessionId owner_id) noexcept;

    void SetStateCallback(GatewayStateCallback callback);
    void PollTimeouts();
    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Reconnect();
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] std::size_t CacheEntryCount() const noexcept;
    [[nodiscard]] std::size_t CacheByteCount() const noexcept;
    [[nodiscard]] std::size_t CacheAdmissionDropCount() const noexcept;
    [[nodiscard]] std::size_t CachePolicyBypassCount() const noexcept;
    [[nodiscard]] std::size_t CacheEvictionCount() const noexcept;
    void Shutdown() noexcept;

private:
    struct CachedValue final {
        std::uint64_t sequence{0U};
        api::VehiclePropertyValue value;
        common::Clock::TimePoint received_at;
        std::size_t accounted_bytes{0U};
        std::uint64_t last_access_order{0U};
        bool fresh{true};
    };

    [[nodiscard]] bool HasActiveSubscriptionLocked(api::PropertyKey key) const;
    [[nodiscard]] bool HasExplicitCachePolicyLocked(api::PropertyKey key) const;
    [[nodiscard]] std::chrono::milliseconds CacheMaxAgeLocked(api::PropertyKey key) const;
    [[nodiscard]] std::uint64_t NextCacheAccessOrderLocked() noexcept;
    void RemoveCacheEntryLocked(std::map<api::PropertyKey, CachedValue>::iterator entry) noexcept;
    void RemoveUnusedKeyStateLocked(api::PropertyKey key) noexcept;
    void OnEvent(api::PropertyEvent event);
    void OnTransportState(bool connected, api::VehicleError error);

    hal::VehicleHalAdapter& hal_adapter_;
    common::Clock& clock_;
    const std::map<std::uint32_t, std::chrono::milliseconds> cache_max_age_by_property_;
    const std::chrono::milliseconds default_cache_max_age_;
    const GatewayCacheLimits cache_limits_;
    std::mutex subscription_mutex_;
    mutable std::mutex mutex_;
    std::map<api::PropertyKey, CachedValue> cache_;
    std::map<api::PropertyKey, std::uint64_t> subscribed_event_sequences_;
    std::map<api::SubscriberId, std::map<api::PropertyKey, ClientEventCallback>> callbacks_;
    GatewayStateCallback state_callback_;
    std::size_t cache_bytes_{0U};
    std::size_t cache_admission_drops_{0U};
    std::size_t cache_policy_bypasses_{0U};
    std::size_t cache_evictions_{0U};
    std::uint64_t cache_access_order_{0U};
    bool shutdown_{false};
};

}  // namespace fw03::middleware
