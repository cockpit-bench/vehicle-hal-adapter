# Vehicle HAL IPC contract 1.0.0

## Scope and definition

`contract/vehicle_hal_contract.proto` is the normative embedded interface definition for capability
negotiation, property get/set, subscriptions, responses, and the event stream. Its package is
`fw03.vehicle.v1`. The C++ API exports the same types and stable numeric error codes from
`contract/include/fw03/api`.

The `host_posix` adapter uses a length-prefixed, bounded binary representation of those logical
messages so host validation has no target runtime dependency. Target adapters must use the target's
supported protobuf/IDL runtime or a reviewed codec with conformance vectors; they must not change
the service, middleware, or HAL interfaces.

Every stream begins with `CapabilityHello(ApiVersion)`. No property request is accepted until the
peer returns `CapabilityHelloAck`. Both sides require the same major version and must accept the
other side's major through `min_compatible_major`. The negotiated minor and patch are the lower
values supported by both peers.

## Parameters and constraints

| Field | Constraint |
|---|---|
| `request_id` | Non-zero unsigned 64-bit value, unique among current pending requests |
| `property_id` | Non-zero unsigned 32-bit vehicle property identifier |
| `area_id` | Unsigned 32-bit area; zero denotes a global property |
| `monotonic_timestamp_ns` | Monotonic source timestamp; never a wall-clock time |
| `sample_rate_hz` | Finite value in `(0, 100]`; the HAL sends the maximum requested client rate |
| `status` | `AVAILABLE`, `UNAVAILABLE`, or `ERROR`; unknown values reject the frame |
| property value | Exactly one typed bool/int32/int64/float/double/string/bytes payload |
| variable field | At most 1 MiB; over-sized strings, bytes, or frames are rejected |

Set carries a value whose property and area exactly match the request key. Get, subscribe, and
unsubscribe carry no value. Unknown operations, payload tags, status values, framing versions, and
trailing bytes are protocol errors rather than silently accepted extensions.

## Stable errors

| Numeric value | Name | Meaning and retry guidance |
|---:|---|---|
| 0 | `OK` | Request completed |
| 1 | `INVALID_ARGUMENT` | Caller or peer violated a parameter constraint; do not retry unchanged |
| 2 | `NOT_SUPPORTED` | Property or operation is unavailable on the peer |
| 3 | `PERMISSION_DENIED` | Session policy rejected access; do not retry without a new policy |
| 4 | `TIMEOUT` | Deadline elapsed; a late response is discarded by request ID |
| 5 | `TRANSPORT_DOWN` | Connection died; reconnect and subscription replay may recover |
| 6 | `STALE_VALUE` | Cached value is outside the caller's freshness contract |
| 7 | `CANCELLED` | Session or process shut down before completion |
| 8 | `INTERNAL` | Invariant or peer response was incomplete |
| 9 | `INCOMPATIBLE_VERSION` | Major/min-compatible ranges do not intersect |

Numeric values are append-only. Receivers encountering an unknown future error must expose it as
`INTERNAL` while retaining diagnostic context; they must not map it to success.

## Example exchange

1. Client sends hello `{major:1, minor:0, patch:0, min_compatible_major:1}`.
2. Peer acknowledges the same version.
3. Client sends get `{request_id:41, property_id:0x11600207, area_id:0}`.
4. Peer returns response 41 with `OK`, an `AVAILABLE` int32 value, and its monotonic timestamp.
5. Client subscribes to the same key at 10 Hz. The peer acknowledges the control request, then emits
   monotonically sequenced `PropertyEvent` messages until the final subscriber is removed.

On transport death, request 41 would complete once with `TRANSPORT_DOWN`. A response for 41 received
after timeout or cancellation has no pending owner and is discarded.

## Compatibility, upgrade, and rollback

- Patch releases may clarify documentation or validation without changing fields or numeric values.
- Minor releases may add optional fields, RPCs, or enum values. Old senders omit new fields; new
  receivers apply documented defaults. Field numbers and numeric enum values are never reused.
- Major releases may make incompatible changes and must use a new package namespace. The hello gate
  rejects them before business traffic.
- A release keeps the previous descriptor and API baseline. Rollback is safe when all messages used
  by the newer deployment are valid under the older minor baseline; otherwise deployment tooling
  must drain sessions before restoring the older endpoint.

Generate a deterministic descriptor and fingerprint with:

```sh
protoc --proto_path=native/contract --include_imports \
  --descriptor_set_out=contract.pb native/contract/vehicle_hal_contract.proto
sha256sum contract.pb native/contract/vehicle_hal_contract.proto
```

CI saves the descriptor, test JUnit, and coverage reports together. Descriptor comparison rejects
removed fields, reused field numbers, or changed stable error values before release.
