# Native vehicle-property adapter architecture

## Production ownership

The native build has six production ownership modules and one composition executable:

| Module | Owns | May depend on |
|---|---|---|
| `fw03_common` | result type, clock boundary, ordered callback executor | C++17 and threads |
| `fw03_vehicle_contract` | property types, stable errors, semantic versions, wire validation, neutral client-session port | common |
| `fw03_platform_adapter` | downstream HAL transport, authenticated client listener, socket ownership | contract |
| `fw03_vehicle_hal` | pending requests, deadlines, subscription aggregation, reconnect replay | platform, contract, common |
| `fw03_vehicle_middleware` | cache, event sequence filtering, per-client callbacks | HAL |
| `fw03_vehicle_service` | client request dispatch, sessions, permissions, compatibility, callback lifetime | middleware and neutral contract interfaces |
| `vehicle_gateway_daemon` | two-endpoint assembly, signal handling, timeout polling, reconnect backoff | service and host adapter |

Production dependencies point down this table and never back toward application. Platform-specific
headers and socket calls are confined to `platform/src`. A target adapter is enabled only when its
legal SDK and compilable implementation are available; selecting `android_ndk` or `qnx` currently
fails configuration instead of linking a success-returning placeholder.

## Forward request path

1. The platform listener authenticates UNIX peer credentials, requires a version hello, and creates
   one `VehicleClientSession` with explicit readable and writable property sets.
2. The client dispatcher rejects replayed request IDs, then routes get or set into
   `VehiclePropertyGateway`; an available cache entry may satisfy a preferred-cache
   get, while an uncached request continues to the HAL.
3. `VehicleHalAdapter` allocates a non-zero request ID, records the deadline and completion owner,
   validates the contract, and sends through `VehicleTransport`.
4. The selected downstream platform adapter serializes a bounded frame and transfers it to the
   vehicle peer.
5. A matching HAL response removes the pending request exactly once and is remapped to the original
   client request ID. An expired request is removed by
   `PollTimeouts`; a later response with the old ID is ignored.
6. The client connection serializes the response on its own writer lock. A disconnected client
   closes its service session and releases all subscriptions without retaining callback ownership.

## Reverse event and death path

The transport reader hands a decoded event to the HAL. A single callback executor orders HAL
delivery. The gateway rejects duplicate and out-of-order sequences, updates its cache, and fans the
event out only to sessions subscribed to the property key. Session closure removes every owned
subscription; the HAL transmits a new effective maximum rate, or an unsubscribe when the final
client leaves.

EOF, protocol violation, or socket failure transitions the transport once to down. The HAL fails all
pending requests with `TRANSPORT_DOWN` and reports the state to every live service session. Desired
subscriptions remain in the registry. After a successful capability handshake, reconnect replays
one effective subscription per property and does not synthesize an event, preventing duplicate
client callbacks.

## Concurrency and shutdown

- Each POSIX reader thread owns blocking reads. A separate send mutex prevents interleaved frames;
  endpoint close uses the same lock before the descriptor can be reused.
- HAL pending and subscription state, gateway cache, and service sessions each have a private mutex;
  callbacks are copied then invoked outside those locks.
- Subscription lifecycle has a dedicated serialization mutex, so service state is never held across
  downstream transport I/O and close cannot leave a ghost subscription.
- The client endpoint refuses regular files and live sockets, removes only a verified stale socket,
  records the bound inode, and unlinks only the endpoint it owns during shutdown.
- `SerialExecutor` is the sole production callback thread owner and supports drain before process
  teardown.
- `Shutdown` is idempotent at service, gateway, HAL, transport, and executor boundaries. Transport
  callbacks are detached before closing the socket, so late I/O cannot enter destroyed business
  objects.

Build and include graphs are emitted through `CMAKE_EXPORT_COMPILE_COMMANDS` and CMake Graphviz.
Tests and downloaded GoogleTest sources are excluded from the six production-module count. The
canonical protobuf descriptor is generated with exact `protoc 25.3` and embedded into the contract
library, making schema drift a compile-time concern rather than an unconsumed documentation file.
