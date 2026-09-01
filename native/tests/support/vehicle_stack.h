#pragma once

#include "fw03/application/vehicle_service.h"
#include "fw03/common/task_executor.h"
#include "fw03/hal/vehicle_hal_adapter.h"
#include "fw03/middleware/vehicle_property_gateway.h"
#include "support/manual_clock.h"
#include "support/mock_vehicle_transport.h"

#include <gmock/gmock.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace fw03::test {

inline constexpr std::uint32_t kVehicleSpeedProperty = 0x11600207U;
inline constexpr std::uint32_t kCabinTemperatureProperty = 0x15400500U;
inline constexpr api::PropertyKey kVehicleSpeedKey{kVehicleSpeedProperty, 0U};
inline constexpr api::PropertyKey kCabinTemperatureKey{kCabinTemperatureProperty, 1U};

inline api::VehiclePropertyValue IntValue(
    api::PropertyKey key,
    std::int32_t value,
    std::int64_t timestamp_ns) {
    return {key, timestamp_ns, api::PropertyStatus::kAvailable, value};
}

class VehicleStack final {
public:
    VehicleStack()
        : transport(std::make_shared<testing::NiceMock<MockVehicleTransport>>()),
          hal(transport, clock, executor),
          gateway(hal),
          service(gateway) {}

    [[nodiscard]] common::Result<api::ApiVersion, api::VehicleError> Start() {
        return service.Start();
    }

    [[nodiscard]] common::Result<api::SessionId, api::VehicleError> OpenSession(
        std::string name,
        application::SessionCallbacks callbacks = {}) {
        if (!callbacks.on_property_event) {
            callbacks.on_property_event = [](api::PropertyEvent) {};
        }
        return service.OpenSession(
            {std::move(name),
             {kVehicleSpeedProperty, kCabinTemperatureProperty},
             {kVehicleSpeedProperty, kCabinTemperatureProperty}},
            api::CurrentApiVersion(),
            std::move(callbacks));
    }

    std::shared_ptr<testing::NiceMock<MockVehicleTransport>> transport;
    ManualClock clock;
    common::InlineExecutor executor;
    hal::VehicleHalAdapter hal;
    middleware::VehiclePropertyGateway gateway;
    application::VehicleService service;
};

}  // namespace fw03::test
