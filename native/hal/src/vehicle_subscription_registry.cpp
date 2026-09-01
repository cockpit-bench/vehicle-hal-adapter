#include "fw03/hal/vehicle_subscription_registry.h"

#include <algorithm>
#include <cmath>

namespace fw03::hal {
namespace {

float EffectiveRate(const std::map<api::SubscriberId, float>& rates) {
    float maximum = 0.0F;
    for (const auto& entry : rates) {
        maximum = std::max(maximum, entry.second);
    }
    return maximum;
}

}  // namespace

common::Result<SubscriptionChange, api::VehicleError>
VehicleSubscriptionRegistry::AddOrUpdate(
    api::SubscriberId subscriber_id,
    api::PropertyKey key,
    float sample_rate_hz) {
    if (subscriber_id == 0U || key.property_id == 0U || !std::isfinite(sample_rate_hz) ||
        sample_rate_hz <= 0.0F || sample_rate_hz > 100.0F) {
        return common::Result<SubscriptionChange, api::VehicleError>::Failure(
            {api::VehicleErrorCode::kInvalidArgument,
             "subscription requires non-zero IDs and a rate in (0, 100] Hz", 0U});
    }

    auto& rates = subscriptions_[key];
    const float previous_rate = EffectiveRate(rates);
    rates[subscriber_id] = sample_rate_hz;
    const float effective_rate = EffectiveRate(rates);
    return common::Result<SubscriptionChange, api::VehicleError>::Success(
        {previous_rate != effective_rate, false, effective_rate});
}

SubscriptionChange VehicleSubscriptionRegistry::Remove(
    api::SubscriberId subscriber_id,
    api::PropertyKey key) {
    const auto property = subscriptions_.find(key);
    if (property == subscriptions_.end()) {
        return {};
    }

    const float previous_rate = EffectiveRate(property->second);
    property->second.erase(subscriber_id);
    if (property->second.empty()) {
        subscriptions_.erase(property);
        return {true, true, 0.0F};
    }

    const float effective_rate = EffectiveRate(property->second);
    return {previous_rate != effective_rate, false, effective_rate};
}

std::vector<EffectiveSubscription> VehicleSubscriptionRegistry::Snapshot() const {
    std::vector<EffectiveSubscription> result;
    result.reserve(subscriptions_.size());
    for (const auto& property : subscriptions_) {
        result.push_back({property.first, EffectiveRate(property.second)});
    }
    return result;
}

void VehicleSubscriptionRegistry::Clear() noexcept { subscriptions_.clear(); }

}  // namespace fw03::hal
