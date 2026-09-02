#include "fw03/application/vehicle_service.h"
#include "fw03/application/vehicle_client_session.h"
#include "fw03/application/daemon_options.h"
#include "fw03/common/clock.h"
#include "fw03/common/task_executor.h"
#include "fw03/hal/vehicle_hal_adapter.h"
#include "fw03/middleware/vehicle_property_gateway.h"
#include "fw03/platform/client_ipc_server.h"
#include "fw03/platform/vehicle_transport.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

void RequestStop(int /*signal_number*/) { g_stop_requested.store(true); }

}  // namespace

int main(int argc, char** argv) {
    const auto parsed_options = fw03::application::ParseDaemonOptions(argc, argv);
    if (!parsed_options) {
        std::cerr << parsed_options.error() << '\n'
                  << fw03::application::DaemonUsage() << '\n';
        return 64;
    }
    auto options = parsed_options.value();
    if (!options.expected_hal_user_id.has_value()) {
        options.expected_hal_user_id = static_cast<std::uint32_t>(::geteuid());
    }
    if (!options.expected_hal_group_id.has_value()) {
        options.expected_hal_group_id = static_cast<std::uint32_t>(::getegid());
    }
    if (options.allowed_client_user_ids.empty()) {
        options.allowed_client_user_ids.insert(static_cast<std::uint32_t>(::geteuid()));
    }
    if (options.allowed_client_group_ids.empty()) {
        options.allowed_client_group_ids.insert(static_cast<std::uint32_t>(::getegid()));
    }

    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);

    auto transport = fw03::platform::CreateHostPosixVehicleTransport(
        options.hal_socket,
        {options.expected_hal_user_id, options.expected_hal_group_id, true});
    fw03::common::SteadyClock clock;
    fw03::common::SerialExecutor callback_executor;
    fw03::hal::VehicleHalAdapter hal_adapter(transport, clock, callback_executor);
    fw03::middleware::VehiclePropertyGateway gateway(
        hal_adapter,
        clock,
        {{0x11600207U, std::chrono::milliseconds{250}},
         {0x15400500U, std::chrono::milliseconds{2000}}});
    fw03::application::VehicleService service(gateway);
    fw03::application::PropertyAllowlistPolicy access_policy(
        options.allowed_client_user_ids,
        options.allowed_client_group_ids,
        options.readable_properties,
        options.writable_properties);

    const auto started = service.Start();
    if (!started) {
        std::cerr << "vehicle gateway failed to start: " << started.error().detail << '\n';
        return 2;
    }

    auto client_server = fw03::platform::CreateHostPosixClientIpcServer(
        options.client_socket,
        [&service, &access_policy] {
            return std::make_unique<fw03::application::VehicleClientSession>(
                service,
                access_policy,
                std::chrono::milliseconds(1000));
        });
    const auto listening = client_server->Start();
    if (!listening) {
        std::cerr << "client IPC failed to start: " << listening.error().detail << '\n';
        service.Shutdown();
        callback_executor.Drain();
        callback_executor.Shutdown();
        return 3;
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

    client_server->Shutdown();
    service.Shutdown();
    callback_executor.Drain();
    callback_executor.Shutdown();
    return 0;
}
