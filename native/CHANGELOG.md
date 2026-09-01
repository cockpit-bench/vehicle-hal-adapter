# Vehicle HAL native contract changelog

## 1.0.0

- Establish the versioned vehicle-property get, set, subscribe, unsubscribe, response, event, and
  capability-negotiation contract.
- Establish a host POSIX transport used by the same HAL, middleware, and service path as target
  adapters.
- Define stable error codes and reject incompatible major versions before accepting requests.

Field numbers and numeric error values published in 1.x are reserved permanently. A future minor
release may add optional fields and enum values; it may not reinterpret an existing field or value.
