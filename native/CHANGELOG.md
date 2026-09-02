# Vehicle HAL native contract changelog

## 1.0.1

- Add the production client-facing UNIX-domain IPC server and route authenticated client get, set,
  subscribe, unsubscribe, responses, and events through the existing service-to-HAL stack.
- Bind the canonical protobuf descriptor into the contract library with exact `protoc 25.3` and
  exercise it in integration tests; no protobuf runtime is linked into the target process.
- Serialize subscription lifecycle updates without holding the service state mutex across transport
  I/O, and serialize socket close against in-flight writes to prevent descriptor reuse races.
- Preserve backward compatibility with the 1.0 contract: field numbers, operations, and stable error
  values are unchanged.

## 1.0.0

- Establish the versioned vehicle-property get, set, subscribe, unsubscribe, response, event, and
  capability-negotiation contract.
- Establish a host POSIX transport used by the same HAL, middleware, and service path as target
  adapters.
- Define stable error codes and reject incompatible major versions before accepting requests.

Field numbers and numeric error values published in 1.x are reserved permanently. A future minor
release may add optional fields and enum values; it may not reinterpret an existing field or value.
