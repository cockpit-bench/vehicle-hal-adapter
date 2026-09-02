#pragma once

#include "fw03/common/result.h"
#include "fw03/api/vehicle_error.h"
#include "fw03/api/vehicle_property.h"

#include <map>
#include <vector>

namespace fw03::hal {

struct SubscriptionChange final {
    bool transport_update_required{false};
    bool transport_unsubscribe_required{false};
    float effective_rate_hz{0.0F};
};

struct EffectiveSubscription final {
    api::PropertyKey key;
    float sample_rate_hz{0.0F};
};

struct SubscriptionUpdate final {
    api::PropertyKey key;
    SubscriptionChange change;
};

class VehicleSubscriptionRegistry final {
public:
    [[nodiscard]] common::Result<SubscriptionChange, api::VehicleError> AddOrUpdate(
        api::SubscriberId subscriber_id,
        api::PropertyKey key,
        float sample_rate_hz);

    [[nodiscard]] SubscriptionChange Remove(
        api::SubscriberId subscriber_id,
        api::PropertyKey key);

    [[nodiscard]] std::vector<SubscriptionUpdate> RemoveSubscriber(
        api::SubscriberId subscriber_id);

    [[nodiscard]] std::vector<EffectiveSubscription> Snapshot() const;
    void Clear() noexcept;

private:
    using SubscriberRates = std::map<api::SubscriberId, float>;
    std::map<api::PropertyKey, SubscriberRates> subscriptions_;
};

}  // namespace fw03::hal
