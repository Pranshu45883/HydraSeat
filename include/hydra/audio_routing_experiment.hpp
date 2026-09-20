#pragma once

#include "hydra/process_identity.hpp"

#include <cstdint>
#include <string>

namespace hydra::windows {

enum class AudioRoutingProbeStatus : std::uint8_t {
    PersistedPolicyRoundTripVerified,
    InvalidTargetProcess,
    TargetAudioSessionNotObserved,
    TargetEndpointUnavailable,
    FactoryUnavailable,
    UnsafeWithoutRestorableBaseline,
    ApplyFailed,
    ApplyVerificationFailed,
    RestoreFailed,
    RestoreVerificationFailed
};

struct AudioRoutingProbeResult {
    AudioRoutingProbeStatus status{AudioRoutingProbeStatus::FactoryUnavailable};
    std::int32_t hresult{0};
    bool targetPolicyVerified{false};
    bool rollbackVerified{false};
};

// Manual feasibility probe only. This contract intentionally does not claim
// production routing support or SeatRuntime integration.
class AudioRoutingExperiment {
public:
    // Verifies only a persisted per-process policy round trip:
    // capture exact prior mapping -> apply target -> verify persisted policy ->
    // restore exact prior mapping -> verify rollback.
    //
    // This does NOT prove that a live audio session actually moved endpoints.
    static AudioRoutingProbeResult probePersistedRoute(
        const hydra::runtime::ProcessIdentity& targetProcess,
        const std::wstring& targetEndpointId);
};

} // namespace hydra::windows
