#include "fw03/application/vehicle_service.h"
#include "fw03/common/clock.h"
#include "fw03/common/task_executor.h"
#include "fw03/hal/vehicle_hal_adapter.h"
#include "fw03/middleware/vehicle_property_gateway.h"
#include "fw03/platform/vehicle_transport.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

void RequestStop(int /*signal_number*/) { g_stop_requested.store(true); }

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/fw03-vehicle-hal.sock";
    if (argc == 3 && std::string(argv[1]) == "--socket") {
        socket_path = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: vehicle_gateway_daemon [--socket <unix-domain-path>]\n";
        return 64;
    }

    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);

    auto transport = fw03::platform::CreateHostPosixVehicleTransport(socket_path);
    fw03::common::SteadyClock clock;
    fw03::common::SerialExecutor callback_executor;
    fw03::hal::VehicleHalAdapter hal_adapter(transport, clock, callback_executor);
    fw03::middleware::VehiclePropertyGateway gateway(hal_adapter);
    fw03::application::VehicleService service(gateway);

    const auto started = service.Start();
    if (!started) {
        std::cerr << "vehicle gateway failed to start: " << started.error().detail << '\n';
        return 2;
    }

    auto reconnect_delay = std::chrono::milliseconds(100);
    constexpr auto kMaximumReconnectDelay = std::chrono::milliseconds(5000);
    while (!g_stop_requested.load()) {
        service.PollTimeouts();
        if (!service.IsConnected()) {
            std::this_thread::sleep_for(reconnect_delay);
            const auto reconnected = service.Reconnect();
            if (reconnected) {
                reconnect_delay = std::chrono::milliseconds(100);
            } else {
                reconnect_delay = std::min(reconnect_delay * 2, kMaximumReconnectDelay);
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    service.Shutdown();
    callback_executor.Drain();
    callback_executor.Shutdown();
    return 0;
}
