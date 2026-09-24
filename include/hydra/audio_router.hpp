#pragma once

#include "hydra/process_identity.hpp"

#include <string>
#include <optional>

namespace hydra::runtime {

struct AudioEndpointIdentity {
    std::wstring endpointId;
    std::optional<std::wstring> stableId;

    bool valid() const noexcept {
        return !endpointId.empty();
    }

    bool operator==(const AudioEndpointIdentity&) const = default;
};

enum class AudioRouteStatus {
    Success,
    InvalidProcess,
    ProcessNotFound,
    AudioSessionNotFound,
    EndpointNotFound,
    EndpointUnavailable,
    IdentityMismatch,
    RoutingFailed,
    OsApiError
};

class AudioRouter {
public:
    virtual ~AudioRouter() = default;

    // Route an application's audio to a specific endpoint
    virtual AudioRouteStatus assignEndpoint(
        const ProcessIdentity& process,
        const AudioEndpointIdentity& endpoint) noexcept = 0;

    // Reset routing for a process back to system default
    virtual AudioRouteStatus clearAssignment(
        const ProcessIdentity& process) noexcept = 0;
};

} // namespace hydra::runtime
