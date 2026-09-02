#pragma once

#include "fw03/api/client_session_port.h"
#include "fw03/common/result.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace fw03::platform {
using ClientWorkerThreadFactory =
    std::function<std::thread(std::function<void()>)>;

class VehicleClientIpcServer {
public:
    virtual ~VehicleClientIpcServer() = default;

    [[nodiscard]] virtual common::Result<void, api::VehicleError> Start() = 0;
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;
    virtual void Shutdown() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<VehicleClientIpcServer> CreateHostPosixClientIpcServer(
    std::string socket_path,
    api::ClientRequestSessionFactory session_factory,
    std::size_t maximum_clients = 8U,
    std::size_t maximum_clients_per_user = 4U,
    ClientWorkerThreadFactory worker_thread_factory = {});

}  // namespace fw03::platform
