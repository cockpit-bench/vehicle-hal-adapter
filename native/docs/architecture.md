# Native vehicle-property adapter architecture

## Production ownership

The native build has six production ownership modules and one composition executable:

| Module | Owns | May depend on |
|---|---|---|
| `fw03_common` | result type, clock boundary, ordered callback executor | C++17 and threads |
| `fw03_vehicle_contract` | property types, stable errors, semantic versions, wire validation | common |
| `fw03_platform_adapter` | host UNIX socket calls and future target SDK calls | contract |
| `fw03_vehicle_hal` | pending requests, deadlines, subscription aggregation, reconnect replay | platform, contract, common |
| `fw03_vehicle_middleware` | cache, event sequence filtering, per-client callbacks | HAL |
| `fw03_vehicle_service` | client sessions, permissions, compatibility checks, callback lifetime | middleware |
| `vehicle_gateway_daemon` | concrete assembly, signal handling, timeout polling, reconnect backoff | service and host adapter |

Production dependencies point down this table and never back toward application. Platform-specific
headers and socket calls are confined to `platform/src`. A target adapter is enabled only when its
legal SDK and compilable implementation are available; selecting `android_ndk` or `qnx` currently
fails configuration instead of linking a success-returning placeholder.

## Forward request path

1. A client opens a version-negotiated `VehicleService` session with explicit readable and writable
   property sets.
2. Get or set enters `VehiclePropertyGateway`; an available cache entry may satisfy a preferred-cache
   get, while an uncached request continues to the HAL.
3. `VehicleHalAdapter` allocates a non-zero request ID, records the deadline and completion owner,
   validates the contract, and sends through `VehicleTransport`.
4. The selected platform adapter serializes a bounded frame and transfers it to the vehicle peer.
5. A matching response removes the pending request exactly once. An expired request is removed by
   `PollTimeouts`; a later response with the old ID is ignored.

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

- The POSIX reader thread owns blocking reads. A separate send mutex prevents interleaved frames.
- HAL pending and subscription state, gateway cache, and service sessions each have a private mutex;
  callbacks are copied then invoked outside those locks.
- `SerialExecutor` is the sole production callback thread owner and supports drain before process
  teardown.
- `Shutdown` is idempotent at service, gateway, HAL, transport, and executor boundaries. Transport
  callbacks are detached before closing the socket, so late I/O cannot enter destroyed business
  objects.

Build and include graphs are emitted through `CMAKE_EXPORT_COMPILE_COMMANDS` and CMake Graphviz.
Tests and downloaded GoogleTest sources are excluded from the six production-module count.
