#pragma once

#include "hydra/process_identity.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace hydra::windows {

// Represents the mapped state of a Windows audio session.
enum class AudioSessionState {
    Active,
    Inactive,
    Expired,
    Unknown
};

// Represents a read-only snapshot of an audio session.
struct AudioSessionObservation {
    std::wstring endpointId;
    std::optional<std::wstring> endpointStableId;
    std::uint32_t processId;
    std::optional<hydra::runtime::ProcessIdentity> processIdentity;
    AudioSessionState state;
    std::optional<std::wstring> displayName;
    std::optional<std::wstring> groupingParam;
};



// Detailed error representation for session observation failures.
struct AudioSessionObserverError {
    enum class Code {
        ComNotInitialized,
        EnumeratorCreationFailed,
        EndpointEnumerationFailed,
        CollectionReadFailed,
        EndpointReadFailed,
        SessionManagerActivationFailed,
        SessionEnumerationFailed,
        InvalidSessionData,
        UnexpectedWindowsState
    };

    Code code;
    std::int32_t hresult;
};

// Encapsulates either a successful list of sessions or an explicit failure.
struct AudioSessionInventoryResult {
    std::vector<AudioSessionObservation> sessions;
    bool isComplete;
    std::optional<AudioSessionObserverError> error;

    bool isSuccess() const noexcept {
        return !error.has_value();
    }
};

// Provides read-only enumeration of Windows Core Audio sessions.
class AudioSessionObserver {
public:
    // Enumerates all current audio sessions across all render endpoints.
    // The calling thread MUST have a valid COM apartment.
    static AudioSessionInventoryResult enumerateSessions();
};

} // namespace hydra::windows
