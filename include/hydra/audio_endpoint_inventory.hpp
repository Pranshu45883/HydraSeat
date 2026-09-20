#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <windows.h> // For HRESULT

namespace hydra::windows {

// Represents the observed state of an audio endpoint from Windows.
enum class AudioEndpointState {
    Active,
    Disabled,
    NotPresent,
    Unplugged,
    Unknown
};

// Represents a snapshot of a Windows audio render endpoint.
// Identity requires endpointId. stableId is optional and available on newer OS versions.
// friendlyName is presentation metadata and MUST NOT be used for identity matching.
struct AudioRenderEndpoint {
    std::wstring endpointId;
    std::optional<std::wstring> stableId;
    std::wstring friendlyName;
    AudioEndpointState state;

    bool isAvailable() const noexcept {
        return state == AudioEndpointState::Active;
    }
};

// Detailed error representation for inventory enumeration failures.
struct AudioEndpointInventoryError {
    enum class Code {
        ComNotInitialized,
        ComInitializationConflict,
        EnumeratorCreationFailed,
        EnumerationFailed,
        CollectionReadFailed,
        EndpointReadFailed,
        PropertyReadFailed,
        UnexpectedWindowsState
    };

    Code code;
    HRESULT hresult;
};

// Encapsulates either a successful list of endpoints or an explicit failure.
struct AudioInventoryResult {
    std::optional<std::vector<AudioRenderEndpoint>> endpoints;
    std::optional<AudioEndpointInventoryError> error;

    bool isSuccess() const noexcept {
        return endpoints.has_value();
    }
};

// Provides read-only enumeration of Windows Core Audio render endpoints.
// Does NOT mutate default audio devices or perform session routing.
class AudioEndpointInventory {
public:
    // Enumerates all render endpoints (Active, Disabled, Unplugged, NotPresent).
    // The calling thread MUST have a valid COM apartment.
    static AudioInventoryResult enumerateRenderEndpoints();
};

} // namespace hydra::windows
