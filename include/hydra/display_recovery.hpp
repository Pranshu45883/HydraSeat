#pragma once

#include "hydra/seat_display_layout.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hydra::display {

enum class RequiredPrimaryLossPolicy : std::uint8_t {
    PauseSession = 0,
    StopAndReturnToWindows = 1,
};

enum class DisplayRecoveryDisposition : std::uint8_t {
    Stable = 0,
    DegradedToSeatPrimary = 1,
    PauseRequired = 2,
    StopRequired = 3,
    RestoreStableLayout = 4,
    Invalid = 5,
};

struct SeatDisplayRecoveryProfile {
    SeatDisplayRequest request;
    RequiredPrimaryLossPolicy primaryLossPolicy{RequiredPrimaryLossPolicy::PauseSession};
};

struct SeatDisplayRecoveryDecision {
    SeatId seatId{0};
    DisplayRecoveryDisposition disposition{DisplayRecoveryDisposition::Invalid};
    bool topologyChanged{false};
    bool degraded{false};
    bool stableIdentityConfirmed{false};
    std::vector<std::string> missingRequiredOutputs;
    std::vector<std::string> missingOptionalOutputs;
    std::vector<std::string> diagnostics;
    SeatDisplayGroup resolvedGroup;
};

SeatDisplayRecoveryDecision planSeatDisplayRecovery(
    const DisplayTopologySnapshot& previousTopology,
    const DisplayTopologySnapshot& currentTopology,
    const SeatDisplayRecoveryProfile& profile,
    const SeatDisplayGroup* previousResolvedGroup = nullptr);

struct DisplayTopologyDebounceState {
    std::uint64_t lastObservedGeneration{0};
    std::uint64_t lastAcceptedGeneration{0};
    std::uint64_t lastChangeTickMs{0};
};

class DisplayTopologyDebouncer {
public:
    explicit DisplayTopologyDebouncer(std::uint32_t quietPeriodMs = 250u);

    void observe(std::uint64_t generation, std::uint64_t tickMs) noexcept;
    bool ready(std::uint64_t tickMs) const noexcept;
    bool accept(std::uint64_t tickMs, std::uint64_t& generation) noexcept;

private:
    std::uint32_t quietPeriodMs_{250u};
    DisplayTopologyDebounceState state_;
};

} // namespace hydra::display
