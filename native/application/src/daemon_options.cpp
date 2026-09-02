#include "fw03/application/daemon_options.h"

#include "fw03/application/platform_profile.h"

#include <limits>
#include <utility>

namespace fw03::application {
namespace {

bool ParseUnsigned32(const std::string& text, bool allow_zero, std::uint32_t& value) {
    try {
        std::size_t consumed = 0U;
        const auto parsed = std::stoull(text, &consumed, 0);
        if (consumed != text.size() || (!allow_zero && parsed == 0U) ||
            parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParsePropertyKey(const std::string& text, api::PropertyKey& key) {
    const auto separator = text.find(':');
    const auto property_text = text.substr(0U, separator);
    const auto area_text = separator == std::string::npos ? std::string{} : text.substr(separator + 1U);
    if (!ParseUnsigned32(property_text, false, key.property_id)) {
        return false;
    }
    if (separator == std::string::npos) {
        key.area_id = 0U;
        return true;
    }
    return !area_text.empty() && ParseUnsigned32(area_text, true, key.area_id);
}

}  // namespace

common::Result<DaemonOptions, std::string> ParseDaemonOptions(
    int argc,
    char* const argv[]) {
    if (argc < 1 || argv == nullptr) {
        return common::Result<DaemonOptions, std::string>::Failure(
            "argument vector is missing");
    }

    DaemonOptions options{
        profile::kDefaultHalSocket,
        profile::kDefaultClientSocket,
        std::nullopt,
        std::nullopt,
        {},
        {},
        {{0x11600207U, 0U}, {0x15400500U, 1U}},
        {{0x15400500U, 1U}}};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] == nullptr ? "" : argv[index];
        if ((argument == "--hal-socket" || argument == "--socket") && index + 1 < argc) {
            options.hal_socket = argv[++index] == nullptr ? "" : argv[index];
            continue;
        }
        if (argument == "--client-socket" && index + 1 < argc) {
            options.client_socket = argv[++index] == nullptr ? "" : argv[index];
            continue;
        }
        if ((argument == "--hal-uid" || argument == "--hal-gid" ||
             argument == "--client-uid" ||
             argument == "--client-gid") &&
            index + 1 < argc) {
            std::uint32_t identity = 0U;
            const std::string value = argv[++index] == nullptr ? "" : argv[index];
            if (!ParseUnsigned32(value, true, identity)) {
                return common::Result<DaemonOptions, std::string>::Failure(
                    "invalid uid/gid: " + value);
            }
            if (argument == "--hal-uid") {
                options.expected_hal_user_id = identity;
            } else if (argument == "--hal-gid") {
                options.expected_hal_group_id = identity;
            } else if (argument == "--client-uid") {
                options.allowed_client_user_ids.insert(identity);
            } else {
                options.allowed_client_group_ids.insert(identity);
            }
            continue;
        }
        if ((argument == "--read-property" || argument == "--write-property") &&
            index + 1 < argc) {
            api::PropertyKey key;
            const std::string value = argv[++index] == nullptr ? "" : argv[index];
            if (!ParsePropertyKey(value, key)) {
                return common::Result<DaemonOptions, std::string>::Failure(
                    "invalid property identifier or area: " + value);
            }
            if (argument == "--read-property") {
                options.readable_properties.insert(key);
            } else {
                options.writable_properties.insert(key);
            }
            continue;
        }
        return common::Result<DaemonOptions, std::string>::Failure(
            "unknown or incomplete argument: " + argument);
    }
    if (options.hal_socket.empty() || options.client_socket.empty()) {
        return common::Result<DaemonOptions, std::string>::Failure(
            "HAL and client socket paths must be non-empty");
    }
    if (options.hal_socket == options.client_socket) {
        return common::Result<DaemonOptions, std::string>::Failure(
            "HAL and client socket paths must be distinct");
    }
    return common::Result<DaemonOptions, std::string>::Success(std::move(options));
}

const char* DaemonUsage() noexcept {
    return "usage: vehicle_gateway_daemon [--hal-socket <path>] "
           "[--client-socket <path>] [--hal-uid <uid>] [--hal-gid <gid>] "
           "[--client-uid <uid>] "
           "[--client-gid <gid>] [--read-property <id[:area]>] "
           "[--write-property <id[:area]>]";
}

}  // namespace fw03::application
