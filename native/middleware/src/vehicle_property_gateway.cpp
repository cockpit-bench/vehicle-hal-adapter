#include "fw03/middleware/vehicle_property_gateway.h"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>

namespace fw03::middleware {
namespace {

constexpr std::size_t kCacheEntryAccountingOverhead = 128U;
constexpr std::size_t kMaximumVariablePayloadBytes = 1024U * 1024U;

GatewayCacheLimits NormalizeCacheLimits(GatewayCacheLimits requested) noexcept {
    requested.maximum_entries =
        std::max<std::size_t>(1U, std::min(requested.maximum_entries,
                                          kGatewayCacheHardMaximumEntries));
    requested.maximum_total_bytes =
        std::max<std::size_t>(1U, std::min(requested.maximum_total_bytes,
                                          kGatewayCacheHardMaximumTotalBytes));
    requested.maximum_entry_bytes = std::max<std::size_t>(
        1U,
        std::min({requested.maximum_entry_bytes,
                  kGatewayCacheHardMaximumEntryBytes,
                  requested.maximum_total_bytes}));
    return requested;
}

std::size_t PayloadStorageBytes(const api::PropertyPayload& payload) noexcept {
    return std::visit(
        [](const auto& candidate) -> std::size_t {
            using Payload = std::decay_t<decltype(candidate)>;
            if constexpr (std::is_same_v<Payload, std::string> ||
                          std::is_same_v<Payload, std::vector<std::uint8_t>>) {
                return candidate.size();
            }
            return sizeof(Payload);
        },
        payload);
}

bool PayloadIsWithinContractLimit(const api::PropertyPayload& payload) noexcept {
    return std::visit(
        [](const auto& candidate) {
            using Payload = std::decay_t<decltype(candidate)>;
            if constexpr (std::is_same_v<Payload, std::string> ||
                          std::is_same_v<Payload, std::vector<std::uint8_t>>) {
                return candidate.size() <= kMaximumVariablePayloadBytes;
            }
            return true;
        },
        payload);
}

std::size_t AccountedValueBytes(const api::VehiclePropertyValue& value) noexcept {
    return kCacheEntryAccountingOverhead + PayloadStorageBytes(value.payload);
}

}  // namespace

VehiclePropertyGateway::VehiclePropertyGateway(
    hal::VehicleHalAdapter& hal_adapter,
    common::Clock& clock,
    std::map<std::uint32_t, std::chrono::milliseconds> cache_max_age_by_property,
    std::chrono::milliseconds default_cache_max_age,
    GatewayCacheLimits cache_limits)
    : hal_adapter_(hal_adapter),
      clock_(clock),
      cache_max_age_by_property_(std::move(cache_max_age_by_property)),
      default_cache_max_age_(default_cache_max_age),
      cache_limits_(NormalizeCacheLimits(cache_limits)) {
    hal_adapter_.SetEventCallback(
        [this](api::PropertyEvent event) { OnEvent(std::move(event)); });
    hal_adapter_.SetTransportStateCallback(
        [this](bool connected, api::VehicleError error) {
            OnTransportState(connected, std::move(error));
        });
}

VehiclePropertyGateway::~VehiclePropertyGateway() { Shutdown(); }

common::Result<api::ApiVersion, api::VehicleError> VehiclePropertyGateway::Start(
    const api::ApiVersion& requested_version) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<api::ApiVersion, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "property gateway is shut down", 0U});
        }
    }
    return hal_adapter_.Start(requested_version);
}

common::Result<api::RequestId, api::VehicleError> VehiclePropertyGateway::Get(
    api::PropertyKey key,
    std::chrono::milliseconds timeout,
    ValueCompletion completion,
    bool prefer_cache,
    api::SessionId owner_id) {
    if (!completion) {
        return common::Result<api::RequestId, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "get completion is required", 0U});
    }
    if (prefer_cache) {
        std::optional<api::VehiclePropertyValue> cached;
        bool stale = false;
        const auto now = clock_.Now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto item = cache_.find(key);
            const auto max_age = CacheMaxAgeLocked(key);
            if (item != cache_.end() &&
                item->second.value.status == api::PropertyStatus::kAvailable &&
                max_age > std::chrono::milliseconds::zero()) {
                if (item->second.fresh && now >= item->second.received_at &&
                    now - item->second.received_at <= max_age) {
                    cached = item->second.value;
                    item->second.last_access_order = NextCacheAccessOrderLocked();
                } else {
                    stale = true;
                }
            }
        }
        if (cached.has_value()) {
            completion(ValueResult::Success(std::move(cached)));
            return common::Result<api::RequestId, api::VehicleError>::Success(0U);
        }
        if (stale && !hal_adapter_.IsConnected()) {
            completion(ValueResult::Failure(
                {api::VehicleErrorCode::kStaleValue,
                 "cached vehicle value exceeded its freshness policy while HAL was offline", 0U}));
            return common::Result<api::RequestId, api::VehicleError>::Success(0U);
        }
    }
    return hal_adapter_.Get(key, timeout, std::move(completion), owner_id);
}

common::Result<api::RequestId, api::VehicleError> VehiclePropertyGateway::Set(
    api::VehiclePropertyValue value,
    std::chrono::milliseconds timeout,
    ValueCompletion completion,
    api::SessionId owner_id) {
    return hal_adapter_.Set(std::move(value), timeout, std::move(completion), owner_id);
}

common::Result<void, api::VehicleError> VehiclePropertyGateway::Subscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    float sample_rate_hz,
    ClientEventCallback callback,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> subscription_lock(subscription_mutex_);
    if (!callback) {
        return common::Result<void, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument, "subscription callback is required", 0U});
    }
    std::optional<ClientEventCallback> previous_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return common::Result<void, api::VehicleError>::Failure(
                {api::VehicleErrorCode::kCancelled, "property gateway is shut down", 0U});
        }
        auto& subscriber = callbacks_[subscriber_id];
        const auto previous = subscriber.find(key);
        if (previous != subscriber.end()) {
            previous_callback = previous->second;
        }
        subscriber[key] = std::move(callback);
    }

    const auto subscribed = hal_adapter_.Subscribe(subscriber_id, key, sample_rate_hz, timeout);
    if (!subscribed) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto subscriber = callbacks_.find(subscriber_id);
        if (subscriber != callbacks_.end()) {
            if (previous_callback.has_value()) {
                subscriber->second[key] = std::move(*previous_callback);
            } else {
                subscriber->second.erase(key);
            }
            if (subscriber->second.empty()) {
                callbacks_.erase(subscriber);
            }
            RemoveUnusedKeyStateLocked(key);
        }
    }
    return subscribed;
}

common::Result<void, api::VehicleError> VehiclePropertyGateway::Unsubscribe(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> subscription_lock(subscription_mutex_);
    const auto unsubscribed = hal_adapter_.Unsubscribe(subscriber_id, key, timeout);
    if (!unsubscribed) {
        return unsubscribed;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto subscriber = callbacks_.find(subscriber_id);
    if (subscriber != callbacks_.end()) {
        subscriber->second.erase(key);
        if (subscriber->second.empty()) {
            callbacks_.erase(subscriber);
        }
    }
    RemoveUnusedKeyStateLocked(key);
    return common::Result<void, api::VehicleError>::Success();
}

common::Result<void, api::VehicleError> VehiclePropertyGateway::ReleaseSubscriber(
    api::SubscriberId subscriber_id,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> subscription_lock(subscription_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto subscriber = callbacks_.find(subscriber_id);
        std::vector<api::PropertyKey> released_keys;
        if (subscriber != callbacks_.end()) {
            released_keys.reserve(subscriber->second.size());
            for (const auto& callback : subscriber->second) {
                released_keys.push_back(callback.first);
            }
        }
        callbacks_.erase(subscriber_id);
        for (const auto key : released_keys) {
            RemoveUnusedKeyStateLocked(key);
        }
    }
    return hal_adapter_.ReleaseSubscriber(subscriber_id, timeout);
}

void VehiclePropertyGateway::CancelOwner(api::SessionId owner_id) noexcept {
    hal_adapter_.CancelOwner(
        owner_id,
        {api::VehicleErrorCode::kCancelled, "client session closed", 0U});
}

void VehiclePropertyGateway::SetStateCallback(GatewayStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callback_ = std::move(callback);
}

void VehiclePropertyGateway::PollTimeouts() { hal_adapter_.PollTimeouts(); }

common::Result<api::ApiVersion, api::VehicleError> VehiclePropertyGateway::Reconnect() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A reconnect attempt creates a new event generation.  Invalidate before HAL Connect and
        // subscription replay can accept events so replay-time values survive a successful commit.
        // On failure the conservative empty-cache state is retained; no value from the dead
        // generation is resurrected or allowed to suppress a later event sequence.
        cache_.clear();
        subscribed_event_sequences_.clear();
        cache_bytes_ = 0U;
    }
    return hal_adapter_.Reconnect();
}

bool VehiclePropertyGateway::IsConnected() const noexcept { return hal_adapter_.IsConnected(); }

std::size_t VehiclePropertyGateway::CacheEntryCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

std::size_t VehiclePropertyGateway::CacheByteCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_bytes_;
}

std::size_t VehiclePropertyGateway::CacheAdmissionDropCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_admission_drops_;
}

std::size_t VehiclePropertyGateway::CachePolicyBypassCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_policy_bypasses_;
}

std::size_t VehiclePropertyGateway::CacheEvictionCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_evictions_;
}

bool VehiclePropertyGateway::HasActiveSubscriptionLocked(api::PropertyKey key) const {
    for (const auto& subscriber : callbacks_) {
        if (subscriber.second.find(key) != subscriber.second.end()) {
            return true;
        }
    }
    return false;
}

bool VehiclePropertyGateway::HasExplicitCachePolicyLocked(api::PropertyKey key) const {
    const auto policy = cache_max_age_by_property_.find(key.property_id);
    return policy != cache_max_age_by_property_.end() &&
           policy->second > std::chrono::milliseconds::zero();
}

std::chrono::milliseconds VehiclePropertyGateway::CacheMaxAgeLocked(
    api::PropertyKey key) const {
    const auto policy = cache_max_age_by_property_.find(key.property_id);
    return policy == cache_max_age_by_property_.end() ? default_cache_max_age_
                                                       : policy->second;
}

std::uint64_t VehiclePropertyGateway::NextCacheAccessOrderLocked() noexcept {
    if (cache_access_order_ != UINT64_MAX) {
        ++cache_access_order_;
    }
    return cache_access_order_;
}

void VehiclePropertyGateway::RemoveCacheEntryLocked(
    std::map<api::PropertyKey, CachedValue>::iterator entry) noexcept {
    if (entry == cache_.end()) {
        return;
    }
    cache_bytes_ = entry->second.accounted_bytes > cache_bytes_
                       ? 0U
                       : cache_bytes_ - entry->second.accounted_bytes;
    cache_.erase(entry);
}

void VehiclePropertyGateway::RemoveUnusedKeyStateLocked(api::PropertyKey key) noexcept {
    if (HasActiveSubscriptionLocked(key) || HasExplicitCachePolicyLocked(key)) {
        return;
    }
    RemoveCacheEntryLocked(cache_.find(key));
    subscribed_event_sequences_.erase(key);
}

void VehiclePropertyGateway::OnEvent(api::PropertyEvent event) {
    std::vector<ClientEventCallback> callbacks;
    if (!PayloadIsWithinContractLimit(event.value.payload)) {
        return;
    }
    const auto received_at = clock_.Now();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            received_at.time_since_epoch())
                            .count();
    constexpr auto kFutureTolerance = std::chrono::milliseconds{100};
    if (event.value.monotonic_timestamp_ns < 0 ||
        event.value.monotonic_timestamp_ns >
            now_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(kFutureTolerance).count()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        const bool has_active_subscription = HasActiveSubscriptionLocked(event.value.key);
        const bool has_explicit_cache_policy =
            HasExplicitCachePolicyLocked(event.value.key);
        if (!has_active_subscription && !has_explicit_cache_policy) {
            ++cache_policy_bypasses_;
            return;
        }
        const auto cached = cache_.find(event.value.key);
        if (cached != cache_.end()) {
            if (event.sequence <= cached->second.sequence) {
                return;
            }
            if (event.value == cached->second.value) {
                cached->second.sequence = event.sequence;
                if (has_active_subscription) {
                    subscribed_event_sequences_[event.value.key] = event.sequence;
                }
                return;
            }
        }
        const auto subscribed_sequence = subscribed_event_sequences_.find(event.value.key);
        if (subscribed_sequence != subscribed_event_sequences_.end() &&
            event.sequence <= subscribed_sequence->second) {
            return;
        }
        if (has_active_subscription) {
            subscribed_event_sequences_[event.value.key] = event.sequence;
        }

        for (const auto& subscriber : callbacks_) {
            const auto callback = subscriber.second.find(event.value.key);
            if (callback != subscriber.second.end()) {
                callbacks.push_back(callback->second);
            }
        }

        const auto max_age = CacheMaxAgeLocked(event.value.key);
        if (max_age <= std::chrono::milliseconds::zero()) {
            ++cache_policy_bypasses_;
            RemoveCacheEntryLocked(cached);
        } else {
            const auto accounted_bytes = AccountedValueBytes(event.value);
            if (accounted_bytes > cache_limits_.maximum_entry_bytes ||
                accounted_bytes > cache_limits_.maximum_total_bytes) {
                ++cache_admission_drops_;
                RemoveCacheEntryLocked(cached);
            } else {
                RemoveCacheEntryLocked(cached);
                while (!cache_.empty() &&
                       (cache_.size() >= cache_limits_.maximum_entries ||
                        cache_bytes_ + accounted_bytes > cache_limits_.maximum_total_bytes)) {
                    const auto victim = std::min_element(
                        cache_.begin(),
                        cache_.end(),
                        [](const auto& lhs, const auto& rhs) {
                            if (lhs.second.last_access_order != rhs.second.last_access_order) {
                                return lhs.second.last_access_order < rhs.second.last_access_order;
                            }
                            return lhs.first < rhs.first;
                        });
                    RemoveCacheEntryLocked(victim);
                    ++cache_evictions_;
                }
                const auto inserted = cache_.emplace(
                    event.value.key,
                    CachedValue{event.sequence,
                                event.value,
                                received_at,
                                accounted_bytes,
                                NextCacheAccessOrderLocked(),
                                true});
                if (inserted.second) {
                    cache_bytes_ += accounted_bytes;
                } else {
                    ++cache_admission_drops_;
                }
            }
        }
    }
    for (auto& callback : callbacks) {
        callback(event);
    }
}

void VehiclePropertyGateway::OnTransportState(bool connected, api::VehicleError error) {
    GatewayStateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        if (!connected) {
            for (auto& item : cache_) {
                item.second.fresh = false;
            }
        }
        callback = state_callback_;
    }
    if (callback) {
        callback(connected, std::move(error));
    }
}

void VehiclePropertyGateway::Shutdown() noexcept {
    std::lock_guard<std::mutex> subscription_lock(subscription_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        callbacks_.clear();
        state_callback_ = {};
        cache_.clear();
        subscribed_event_sequences_.clear();
        cache_bytes_ = 0U;
    }
    hal_adapter_.SetEventCallback({});
    hal_adapter_.SetTransportStateCallback({});
    hal_adapter_.Shutdown();
}

}  // namespace fw03::middleware
