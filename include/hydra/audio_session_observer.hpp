#pragma once

#include "hydra/runtime_authority.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <windows.h> // For HRESULT

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
    std::uint32_t processId;
    std::optional<hydra::runtime::ProcessIdentity> processIdentity;
    AudioSessionState state;
    std::optional<std::wstring> displayName;
    std::optional<std::wstring> groupingParam;
};

// Represents the pure result of comparing an observed identity to an expected identity.
enum class ProcessOwnershipMatch {
    Match,
    Mismatch,
    Unknown
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
    HRESULT hresult;
};

// Encapsulates either a successful list of sessions or an explicit failure.
struct AudioSessionInventoryResult {
    std::optional<std::vector<AudioSessionObservation>> sessions;
    std::optional<AudioSessionObserverError> error;

    bool isSuccess() const noexcept {
        return sessions.has_value();
    }
};

// Provides read-only enumeration of Windows Core Audio sessions.
class AudioSessionObserver {
public:
    // Enumerates all current audio sessions across all render endpoints.
    // The calling thread MUST have a valid COM apartment.
    static AudioSessionInventoryResult enumerateSessions();

    // Pure helper to compare an observed identity with an expected one.
    static ProcessOwnershipMatch matchIdentity(
        const std::optional<hydra::runtime::ProcessIdentity>& observed, 
        const hydra::runtime::ProcessIdentity& expected) noexcept;
};

} // namespace hydra::windows
