#pragma once

#include "hydra/input_metrics.hpp"
#include "hydra/workspace_manager.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::metrics {

inline constexpr std::uint32_t kSessionMetricsSchemaVersion = 1u;
inline constexpr std::size_t kMinimumSessionMetricSeats = 1u;
inline constexpr std::size_t kMaximumSessionMetricSeats = 2u;

enum class EvidenceOrigin : std::uint8_t {
    Synthetic = 0,
    ControlledProcess = 1,
    Physical = 2,
};

enum class EvidenceVerdict : std::uint8_t {
    InsufficientEvidence = 0,
    Pass = 1,
    Fail = 2,
};

enum class CapabilityOutcome : std::uint8_t {
    NotRequired = 0,
    Success = 1,
    Unsupported = 2,
    Failed = 3,
    MissingEvidence = 4,
};

enum class SessionFinalState : std::uint8_t {
    Running = 0,
    ReturnedToWindows = 1,
    RecoveryRequired = 2,
};

struct SeatSessionMetrics {
    SeatId seatId{0};
    std::uint64_t launchDurationMicros{0};
    std::uint64_t stopDurationMicros{0};
    std::uint64_t rollbackDurationMicros{0};
    bool processStarted{false};
    bool windowOwnershipVerified{false};
    bool displayPlacementVerified{false};
    bool inputRouteReady{false};
    CapabilityOutcome controller{CapabilityOutcome::NotRequired};
    CapabilityOutcome audio{CapabilityOutcome::NotRequired};

    bool operator==(const SeatSessionMetrics&) const = default;
};

struct SessionMetricsBuildInput {
    std::uint32_t schemaVersion{kSessionMetricsSchemaVersion};
    std::uint64_t planFingerprint{0};
    EvidenceOrigin origin{EvidenceOrigin::Synthetic};
    InputMetricsSnapshot input;
    std::vector<SeatSessionMetrics> seats;
    SessionFinalState finalState{SessionFinalState::Running};
    bool rollbackAttempted{false};
    bool rollbackVerified{false};
};

struct SessionMetricsReport {
    std::uint32_t schemaVersion{kSessionMetricsSchemaVersion};
    std::uint64_t planFingerprint{0};
    EvidenceOrigin origin{EvidenceOrigin::Synthetic};
    EvidenceVerdict isolationVerdict{EvidenceVerdict::InsufficientEvidence};
    EvidenceVerdict sessionVerdict{EvidenceVerdict::InsufficientEvidence};
    bool physicalValidationEligible{false};
    bool receiverEvidenceComplete{false};
    bool lossFreeEvidence{false};
    InputMetricsReport input;
    std::vector<SeatSessionMetrics> seats;
    SessionFinalState finalState{SessionFinalState::Running};
    bool rollbackAttempted{false};
    bool rollbackVerified{false};
    std::uint64_t maximumLaunchDurationMicros{0};
    std::uint64_t maximumStopDurationMicros{0};
    std::uint64_t maximumRollbackDurationMicros{0};
};

enum class SessionMetricsResult : std::uint8_t {
    Success = 0,
    UnsupportedSchema = 1,
    InvalidPlanFingerprint = 2,
    InvalidSeatCount = 3,
    InvalidSeatId = 4,
    DuplicateSeatId = 5,
    InvalidInputMetrics = 6,
    InvalidRollbackState = 7,
};

SessionMetricsResult buildSessionMetricsReport(
    const SessionMetricsBuildInput& input,
    SessionMetricsReport& report);

std::string encodeSessionMetricsReportJson(const SessionMetricsReport& report);

std::string_view evidenceOriginName(EvidenceOrigin value) noexcept;
std::string_view evidenceVerdictName(EvidenceVerdict value) noexcept;
std::string_view capabilityOutcomeName(CapabilityOutcome value) noexcept;
std::string_view sessionFinalStateName(SessionFinalState value) noexcept;
std::string_view sessionMetricsResultName(SessionMetricsResult value) noexcept;

} // namespace hydra::metrics
