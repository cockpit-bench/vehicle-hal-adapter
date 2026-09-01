#include "fw03/api/wire_codec.h"

#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace fw03::api {
namespace {

constexpr std::uint32_t kWireMagic = 0x46573033U;
constexpr std::uint16_t kWireMajor = 1U;
constexpr std::uint16_t kWireMinor = 0U;
constexpr std::size_t kMaximumVariableFieldBytes = 1024U * 1024U;

enum class WireKind : std::uint8_t {
    kHello = 1U,
    kHelloAck = 2U,
    kRequest = 3U,
    kResponse = 4U,
    kEvent = 5U,
};

template <typename T>
void AppendUnsigned(std::vector<std::uint8_t>& bytes, T value) {
    static_assert(std::is_unsigned<T>::value, "wire integral type must be unsigned");
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & static_cast<T>(0xffU)));
    }
}

void AppendFloat(std::vector<std::uint8_t>& bytes, float value) {
    std::uint32_t encoded = 0U;
    static_assert(sizeof(encoded) == sizeof(value), "float wire width changed");
    std::memcpy(&encoded, &value, sizeof(encoded));
    AppendUnsigned(bytes, encoded);
}

void AppendDouble(std::vector<std::uint8_t>& bytes, double value) {
    std::uint64_t encoded = 0U;
    static_assert(sizeof(encoded) == sizeof(value), "double wire width changed");
    std::memcpy(&encoded, &value, sizeof(encoded));
    AppendUnsigned(bytes, encoded);
}

bool AppendLength(std::vector<std::uint8_t>& bytes, std::size_t length) {
    if (length > kMaximumVariableFieldBytes ||
        length > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    AppendUnsigned(bytes, static_cast<std::uint32_t>(length));
    return true;
}

bool AppendString(std::vector<std::uint8_t>& bytes, const std::string& value) {
    if (!AppendLength(bytes, value.size())) {
        return false;
    }
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

class Reader final {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    template <typename T>
    bool ReadUnsigned(T& value) {
        static_assert(std::is_unsigned<T>::value, "wire integral type must be unsigned");
        if (Remaining() < sizeof(T)) {
            return false;
        }
        std::uint64_t decoded = 0U;
        for (std::size_t index = 0U; index < sizeof(T); ++index) {
            const auto shift = static_cast<unsigned int>(index * 8U);
            decoded |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << shift;
        }
        value = static_cast<T>(decoded);
        offset_ += sizeof(T);
        return true;
    }

    bool ReadFloat(float& value) {
        std::uint32_t encoded = 0U;
        if (!ReadUnsigned(encoded)) {
            return false;
        }
        std::memcpy(&value, &encoded, sizeof(value));
        return true;
    }

    bool ReadDouble(double& value) {
        std::uint64_t encoded = 0U;
        if (!ReadUnsigned(encoded)) {
            return false;
        }
        std::memcpy(&value, &encoded, sizeof(value));
        return true;
    }

    bool ReadString(std::string& value) {
        std::uint32_t length = 0U;
        if (!ReadUnsigned(length) || length > kMaximumVariableFieldBytes ||
            Remaining() < static_cast<std::size_t>(length)) {
            return false;
        }
        const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(offset_);
        const auto last = first + static_cast<std::ptrdiff_t>(length);
        value.assign(first, last);
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    bool ReadBytes(std::vector<std::uint8_t>& value) {
        std::uint32_t length = 0U;
        if (!ReadUnsigned(length) || length > kMaximumVariableFieldBytes ||
            Remaining() < static_cast<std::size_t>(length)) {
            return false;
        }
        const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(offset_);
        const auto last = first + static_cast<std::ptrdiff_t>(length);
        value.assign(first, last);
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return offset_ == bytes_.size(); }

private:
    [[nodiscard]] std::size_t Remaining() const noexcept { return bytes_.size() - offset_; }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_{0U};
};

void EncodeVersion(std::vector<std::uint8_t>& bytes, const ApiVersion& version) {
    AppendUnsigned(bytes, version.major);
    AppendUnsigned(bytes, version.minor);
    AppendUnsigned(bytes, version.patch);
    AppendUnsigned(bytes, version.min_compatible_major);
}

bool DecodeVersion(Reader& reader, ApiVersion& version) {
    return reader.ReadUnsigned(version.major) && reader.ReadUnsigned(version.minor) &&
           reader.ReadUnsigned(version.patch) &&
           reader.ReadUnsigned(version.min_compatible_major);
}

bool EncodeError(std::vector<std::uint8_t>& bytes, const VehicleError& error) {
    AppendUnsigned(bytes, static_cast<std::uint32_t>(error.code));
    AppendUnsigned(bytes, error.request_id);
    return AppendString(bytes, error.detail);
}

bool DecodeError(Reader& reader, VehicleError& error) {
    std::uint32_t code = 0U;
    if (!reader.ReadUnsigned(code) || !reader.ReadUnsigned(error.request_id) ||
        !reader.ReadString(error.detail)) {
        return false;
    }
    if (code > static_cast<std::uint32_t>(VehicleErrorCode::kIncompatibleVersion)) {
        error.code = VehicleErrorCode::kInternal;
        error.detail = "unknown peer error code " + std::to_string(code) + ": " + error.detail;
    } else {
        error.code = static_cast<VehicleErrorCode>(code);
    }
    return true;
}

void EncodeKey(std::vector<std::uint8_t>& bytes, const PropertyKey& key) {
    AppendUnsigned(bytes, key.property_id);
    AppendUnsigned(bytes, key.area_id);
}

bool DecodeKey(Reader& reader, PropertyKey& key) {
    return reader.ReadUnsigned(key.property_id) && reader.ReadUnsigned(key.area_id);
}

bool EncodePayload(std::vector<std::uint8_t>& bytes, const PropertyPayload& payload) {
    if (const auto* value = std::get_if<bool>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(0U));
        AppendUnsigned(bytes, static_cast<std::uint8_t>(*value ? 1U : 0U));
        return true;
    }
    if (const auto* value = std::get_if<std::int32_t>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(1U));
        AppendUnsigned(bytes, static_cast<std::uint32_t>(*value));
        return true;
    }
    if (const auto* value = std::get_if<std::int64_t>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(2U));
        AppendUnsigned(bytes, static_cast<std::uint64_t>(*value));
        return true;
    }
    if (const auto* value = std::get_if<float>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(3U));
        AppendFloat(bytes, *value);
        return true;
    }
    if (const auto* value = std::get_if<double>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(4U));
        AppendDouble(bytes, *value);
        return true;
    }
    if (const auto* value = std::get_if<std::string>(&payload)) {
        AppendUnsigned(bytes, static_cast<std::uint8_t>(5U));
        return AppendString(bytes, *value);
    }
    const auto* value = std::get_if<std::vector<std::uint8_t>>(&payload);
    if (value == nullptr) {
        return false;
    }
    AppendUnsigned(bytes, static_cast<std::uint8_t>(6U));
    if (!AppendLength(bytes, value->size())) {
        return false;
    }
    bytes.insert(bytes.end(), value->begin(), value->end());
    return true;
}

bool DecodePayload(Reader& reader, PropertyPayload& payload) {
    std::uint8_t tag = 0U;
    if (!reader.ReadUnsigned(tag)) {
        return false;
    }
    switch (tag) {
        case 0U: {
            std::uint8_t value = 0U;
            if (!reader.ReadUnsigned(value) || value > 1U) {
                return false;
            }
            payload = value == 1U;
            return true;
        }
        case 1U: {
            std::uint32_t value = 0U;
            if (!reader.ReadUnsigned(value)) {
                return false;
            }
            payload = static_cast<std::int32_t>(value);
            return true;
        }
        case 2U: {
            std::uint64_t value = 0U;
            if (!reader.ReadUnsigned(value)) {
                return false;
            }
            payload = static_cast<std::int64_t>(value);
            return true;
        }
        case 3U: {
            float value = 0.0F;
            if (!reader.ReadFloat(value)) {
                return false;
            }
            payload = value;
            return true;
        }
        case 4U: {
            double value = 0.0;
            if (!reader.ReadDouble(value)) {
                return false;
            }
            payload = value;
            return true;
        }
        case 5U: {
            std::string value;
            if (!reader.ReadString(value)) {
                return false;
            }
            payload = std::move(value);
            return true;
        }
        case 6U: {
            std::vector<std::uint8_t> value;
            if (!reader.ReadBytes(value)) {
                return false;
            }
            payload = std::move(value);
            return true;
        }
        default:
            return false;
    }
}

bool EncodeValue(std::vector<std::uint8_t>& bytes, const VehiclePropertyValue& value) {
    EncodeKey(bytes, value.key);
    AppendUnsigned(bytes, static_cast<std::uint64_t>(value.monotonic_timestamp_ns));
    AppendUnsigned(bytes, static_cast<std::uint8_t>(value.status));
    return EncodePayload(bytes, value.payload);
}

bool DecodeValue(Reader& reader, VehiclePropertyValue& value) {
    std::uint64_t timestamp = 0U;
    std::uint8_t status = 0U;
    if (!DecodeKey(reader, value.key) || !reader.ReadUnsigned(timestamp) ||
        !reader.ReadUnsigned(status) || status > static_cast<std::uint8_t>(PropertyStatus::kError) ||
        !DecodePayload(reader, value.payload)) {
        return false;
    }
    value.monotonic_timestamp_ns = static_cast<std::int64_t>(timestamp);
    value.status = static_cast<PropertyStatus>(status);
    return true;
}

void EncodeHeader(std::vector<std::uint8_t>& bytes, WireKind kind) {
    AppendUnsigned(bytes, kWireMagic);
    AppendUnsigned(bytes, kWireMajor);
    AppendUnsigned(bytes, kWireMinor);
    AppendUnsigned(bytes, static_cast<std::uint8_t>(kind));
}

VehicleError Malformed(std::string detail) {
    return {VehicleErrorCode::kInvalidArgument, std::move(detail), 0U};
}

}  // namespace

common::Result<std::vector<std::uint8_t>, VehicleError> EncodeWireMessage(
    const WireMessage& message) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(128U);

    bool encoded = true;
    if (const auto* hello = std::get_if<Hello>(&message)) {
        EncodeHeader(bytes, WireKind::kHello);
        EncodeVersion(bytes, hello->requested_version);
    } else if (const auto* ack = std::get_if<HelloAck>(&message)) {
        EncodeHeader(bytes, WireKind::kHelloAck);
        encoded = EncodeError(bytes, ack->error);
        EncodeVersion(bytes, ack->negotiated_version);
    } else if (const auto* request = std::get_if<TransportRequest>(&message)) {
        const auto validation = ValidateRequest(*request);
        if (!validation) {
            return common::Result<std::vector<std::uint8_t>, VehicleError>::Failure(validation.error());
        }
        EncodeHeader(bytes, WireKind::kRequest);
        AppendUnsigned(bytes, request->request_id);
        AppendUnsigned(bytes, static_cast<std::uint8_t>(request->operation));
        EncodeKey(bytes, request->key);
        AppendFloat(bytes, request->sample_rate_hz);
        AppendUnsigned(bytes, static_cast<std::uint8_t>(request->value.has_value() ? 1U : 0U));
        if (request->value.has_value()) {
            encoded = EncodeValue(bytes, *request->value);
        }
    } else if (const auto* response = std::get_if<TransportResponse>(&message)) {
        EncodeHeader(bytes, WireKind::kResponse);
        AppendUnsigned(bytes, response->request_id);
        encoded = EncodeError(bytes, response->error);
        AppendUnsigned(bytes, static_cast<std::uint8_t>(response->value.has_value() ? 1U : 0U));
        if (response->value.has_value()) {
            encoded = encoded && EncodeValue(bytes, *response->value);
        }
    } else if (const auto* event = std::get_if<PropertyEvent>(&message)) {
        EncodeHeader(bytes, WireKind::kEvent);
        AppendUnsigned(bytes, event->sequence);
        encoded = EncodeValue(bytes, event->value);
    } else {
        encoded = false;
    }

    if (!encoded) {
        return common::Result<std::vector<std::uint8_t>, VehicleError>::Failure(
            Malformed("wire field exceeds its bounded size"));
    }
    return common::Result<std::vector<std::uint8_t>, VehicleError>::Success(std::move(bytes));
}

common::Result<WireMessage, VehicleError> DecodeWireMessage(
    const std::vector<std::uint8_t>& bytes) {
    Reader reader(bytes);
    std::uint32_t magic = 0U;
    std::uint16_t major = 0U;
    std::uint16_t minor = 0U;
    std::uint8_t raw_kind = 0U;
    if (!reader.ReadUnsigned(magic) || !reader.ReadUnsigned(major) ||
        !reader.ReadUnsigned(minor) || !reader.ReadUnsigned(raw_kind) || magic != kWireMagic ||
        major != kWireMajor || minor > kWireMinor) {
        return common::Result<WireMessage, VehicleError>::Failure(
            Malformed("invalid wire header or unsupported framing version"));
    }

    const auto kind = static_cast<WireKind>(raw_kind);
    WireMessage message;
    switch (kind) {
        case WireKind::kHello: {
            Hello hello;
            if (!DecodeVersion(reader, hello.requested_version)) {
                return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed hello"));
            }
            message = hello;
            break;
        }
        case WireKind::kHelloAck: {
            HelloAck ack;
            if (!DecodeError(reader, ack.error) || !DecodeVersion(reader, ack.negotiated_version)) {
                return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed hello ack"));
            }
            message = std::move(ack);
            break;
        }
        case WireKind::kRequest: {
            TransportRequest request;
            std::uint8_t operation = 0U;
            std::uint8_t has_value = 0U;
            if (!reader.ReadUnsigned(request.request_id) || !reader.ReadUnsigned(operation) ||
                operation < static_cast<std::uint8_t>(TransportOperation::kGet) ||
                operation > static_cast<std::uint8_t>(TransportOperation::kUnsubscribe) ||
                !DecodeKey(reader, request.key) || !reader.ReadFloat(request.sample_rate_hz) ||
                !reader.ReadUnsigned(has_value) || has_value > 1U) {
                return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed request"));
            }
            request.operation = static_cast<TransportOperation>(operation);
            if (has_value == 1U) {
                VehiclePropertyValue value;
                if (!DecodeValue(reader, value)) {
                    return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed request value"));
                }
                request.value = std::move(value);
            }
            const auto validation = ValidateRequest(request);
            if (!validation) {
                return common::Result<WireMessage, VehicleError>::Failure(validation.error());
            }
            message = std::move(request);
            break;
        }
        case WireKind::kResponse: {
            TransportResponse response;
            std::uint8_t has_value = 0U;
            if (!reader.ReadUnsigned(response.request_id) || !DecodeError(reader, response.error) ||
                !reader.ReadUnsigned(has_value) || has_value > 1U) {
                return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed response"));
            }
            if (has_value == 1U) {
                VehiclePropertyValue value;
                if (!DecodeValue(reader, value)) {
                    return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed response value"));
                }
                response.value = std::move(value);
            }
            message = std::move(response);
            break;
        }
        case WireKind::kEvent: {
            PropertyEvent event;
            if (!reader.ReadUnsigned(event.sequence) || !DecodeValue(reader, event.value)) {
                return common::Result<WireMessage, VehicleError>::Failure(Malformed("malformed event"));
            }
            message = std::move(event);
            break;
        }
        default:
            return common::Result<WireMessage, VehicleError>::Failure(Malformed("unknown message kind"));
    }

    if (!reader.empty()) {
        return common::Result<WireMessage, VehicleError>::Failure(Malformed("trailing bytes after message"));
    }
    return common::Result<WireMessage, VehicleError>::Success(std::move(message));
}

}  // namespace fw03::api
