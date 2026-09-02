#pragma once

#include "fw03/api/wire_codec.h"
#include "fw03/common/result.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace fw03::api {

// Neutral inbound IPC port shared by the application policy and platform
// listener.  Keeping this contract outside the platform module prevents the
// application layer from depending on a host/QNX/MCU implementation boundary.
struct PeerCredentials final {
    std::int64_t process_id{-1};
    std::uint32_t user_id{0U};
    std::uint32_t group_id{0U};
};

using ClientMessageSink = std::function<bool(WireMessage)>;

class ClientRequestSession {
public:
    virtual ~ClientRequestSession() = default;

    [[nodiscard]] virtual common::Result<ApiVersion, VehicleError> Open(
        const PeerCredentials& peer,
        const ApiVersion& requested_version,
        ClientMessageSink outbound) = 0;
    virtual void HandleRequest(TransportRequest request) = 0;
    virtual void Close() noexcept = 0;
};

using ClientRequestSessionFactory =
    std::function<std::unique_ptr<ClientRequestSession>()>;

}  // namespace fw03::api
