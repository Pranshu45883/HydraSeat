#include "hydra/session_metrics.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace hydra::metrics {
namespace {

bool capabilityFailed(CapabilityOutcome value) noexcept {
    return value == CapabilityOutcome::Failed;
}

bool capabilityComplete(CapabilityOutcome value) noexcept {
    return value == CapabilityOutcome::NotRequired ||
           value == CapabilityOutcome::Success;
}

bool seatFailed(const SeatSessionMetrics& seat) noexcept {
    return capabilityFailed(seat.controller) || capabilityFailed(seat.audio);
}

bool seatComplete(const SeatSessionMetrics& seat) noexcept {
    return seat.processStarted && seat.windowOwnershipVerified &&
           seat.displayPlacementVerified && seat.inputRouteReady &&
           capabilityComplete(seat.controller) &&
           capabilityComplete(seat.audio);
}

EvidenceVerdict isolationVerdict(const InputMetricsReport& input,
                                 bool receiverComplete,
                                 bool lossFree) noexcept {
    if (input.crossSeatEvents != 0 || input.crossProcessEvents != 0) {
        return EvidenceVerdict::Fail;
    }
    if (!receiverComplete || !lossFree || input.missingStageEvents != 0 ||
        input.completeInputEvents != input.uniqueInputEvents ||
        input.uniqueInputEvents == 0) {
        return EvidenceVerdict::InsufficientEvidence;
    }
    return EvidenceVerdict::Pass;
}

EvidenceVerdict overallVerdict(const SessionMetricsBuildInput& source,
                               const SessionMetricsReport& report) noexcept {
    if (report.isolationVerdict == EvidenceVerdict::Fail ||
        source.finalState == SessionFinalState::RecoveryRequired ||
        (source.rollbackAttempted && !source.rollbackVerified) ||
        (source.finalState == SessionFinalState::ReturnedToWindows &&
         (!source.rollbackAttempted || !source.rollbackVerified)) ||
        std::any_of(source.seats.begin(), source.seats.end(), seatFailed)) {
        return EvidenceVerdict::Fail;
    }
    if (report.isolationVerdict != EvidenceVerdict::Pass ||
        std::any_of(source.seats.begin(), source.seats.end(),
                    [](const SeatSessionMetrics& seat) {
                        return !seatComplete(seat);
                    })) {
        return EvidenceVerdict::InsufficientEvidence;
    }
    return EvidenceVerdict::Pass;
}

void appendBool(std::ostringstream& stream, bool value) {
    stream << (value ? "true" : "false");
}

} // namespace

SessionMetricsResult buildSessionMetricsReport(
    const SessionMetricsBuildInput& source,
    SessionMetricsReport& report) {
    report = {};
    if (source.schemaVersion != kSessionMetricsSchemaVersion) {
        return SessionMetricsResult::UnsupportedSchema;
    }
    if (source.planFingerprint == 0) {
        return SessionMetricsResult::InvalidPlanFingerprint;
    }
    if (source.seats.size() < kMinimumSessionMetricSeats ||
        source.seats.size() > kMaximumSessionMetricSeats) {
        return SessionMetricsResult::InvalidSeatCount;
    }
    if (source.rollbackVerified && !source.rollbackAttempted) {
        return SessionMetricsResult::InvalidRollbackState;
    }

    std::set<SeatId> seatIds;
    for (const auto& seat : source.seats) {
        if (seat.seatId == 0) return SessionMetricsResult::InvalidSeatId;
        if (!seatIds.insert(seat.seatId).second) {
            return SessionMetricsResult::DuplicateSeatId;
        }
    }

    InputMetricsReport inputReport;
    if (buildInputMetricsReport(source.input, inputReport) !=
        InputMetricsReportResult::Success) {
        return SessionMetricsResult::InvalidInputMetrics;
    }

    report.schemaVersion = kSessionMetricsSchemaVersion;
    report.planFingerprint = source.planFingerprint;
    report.origin = source.origin;
    report.input = inputReport;
    report.seats = source.seats;
    std::sort(report.seats.begin(), report.seats.end(),
              [](const SeatSessionMetrics& left,
                 const SeatSessionMetrics& right) {
                  return left.seatId < right.seatId;
              });
    report.finalState = source.finalState;
    report.rollbackAttempted = source.rollbackAttempted;
    report.rollbackVerified = source.rollbackVerified;

    report.receiverEvidenceComplete =
        inputReport.uniqueInputEvents != 0 &&
        inputReport.receiverVerifiedEvents == inputReport.uniqueInputEvents &&
        inputReport.missingReceiverEvidenceEvents == 0;
    report.lossFreeEvidence =
        inputReport.queueDroppedFrames == 0 &&
        inputReport.recorderRotationDrops == 0 &&
        inputReport.recorderContentionDrops == 0 &&
        inputReport.recorderInvalidSamples == 0;
    report.isolationVerdict = isolationVerdict(
        inputReport, report.receiverEvidenceComplete,
        report.lossFreeEvidence);

    for (const auto& seat : report.seats) {
        report.maximumLaunchDurationMicros =
            std::max(report.maximumLaunchDurationMicros,
                     seat.launchDurationMicros);
        report.maximumStopDurationMicros =
            std::max(report.maximumStopDurationMicros,
                     seat.stopDurationMicros);
        report.maximumRollbackDurationMicros =
            std::max(report.maximumRollbackDurationMicros,
                     seat.rollbackDurationMicros);
    }

    report.sessionVerdict = overallVerdict(source, report);
    report.physicalValidationEligible =
        source.origin == EvidenceOrigin::Physical &&
        report.sessionVerdict == EvidenceVerdict::Pass;
    return SessionMetricsResult::Success;
}

std::string encodeSessionMetricsReportJson(const SessionMetricsReport& report) {
    std::ostringstream stream;
    stream << '{'
           << "\"schema_version\":" << report.schemaVersion << ','
           << "\"plan_fingerprint\":" << report.planFingerprint << ','
           << "\"evidence_origin\":\"" << evidenceOriginName(report.origin) << "\","
           << "\"isolation_verdict\":\"" << evidenceVerdictName(report.isolationVerdict) << "\","
           << "\"session_verdict\":\"" << evidenceVerdictName(report.sessionVerdict) << "\","
           << "\"physical_validation_eligible\":";
    appendBool(stream, report.physicalValidationEligible);
    stream << ",\"receiver_evidence_complete\":";
    appendBool(stream, report.receiverEvidenceComplete);
    stream << ",\"loss_free_evidence\":";
    appendBool(stream, report.lossFreeEvidence);
    stream << ",\"final_state\":\"" << sessionFinalStateName(report.finalState) << "\","
           << "\"rollback_attempted\":";
    appendBool(stream, report.rollbackAttempted);
    stream << ",\"rollback_verified\":";
    appendBool(stream, report.rollbackVerified);
    stream << ",\"max_launch_us\":" << report.maximumLaunchDurationMicros
           << ",\"max_stop_us\":" << report.maximumStopDurationMicros
           << ",\"max_rollback_us\":" << report.maximumRollbackDurationMicros
           << ",\"input\":" << encodeInputMetricsReportJson(report.input)
           << ",\"seats\":[";
    for (std::size_t index = 0; index < report.seats.size(); ++index) {
        if (index != 0) stream << ',';
        const auto& seat = report.seats[index];
        stream << '{'
               << "\"seat_id\":" << seat.seatId
               << ",\"launch_us\":" << seat.launchDurationMicros
               << ",\"stop_us\":" << seat.stopDurationMicros
               << ",\"rollback_us\":" << seat.rollbackDurationMicros
               << ",\"process_started\":";
        appendBool(stream, seat.processStarted);
        stream << ",\"window_verified\":";
        appendBool(stream, seat.windowOwnershipVerified);
        stream << ",\"display_verified\":";
        appendBool(stream, seat.displayPlacementVerified);
        stream << ",\"input_route_ready\":";
        appendBool(stream, seat.inputRouteReady);
        stream << ",\"controller\":\"" << capabilityOutcomeName(seat.controller) << "\""
               << ",\"audio\":\"" << capabilityOutcomeName(seat.audio) << "\""
               << '}';
    }
    stream << "]}";
    return stream.str();
}

std::string_view evidenceOriginName(EvidenceOrigin value) noexcept {
    switch (value) {
        case EvidenceOrigin::Synthetic: return "synthetic";
        case EvidenceOrigin::ControlledProcess: return "controlled-process";
        case EvidenceOrigin::Physical: return "physical";
    }
    return "unknown";
}

std::string_view evidenceVerdictName(EvidenceVerdict value) noexcept {
    switch (value) {
        case EvidenceVerdict::InsufficientEvidence: return "insufficient-evidence";
        case EvidenceVerdict::Pass: return "pass";
        case EvidenceVerdict::Fail: return "fail";
    }
    return "unknown";
}

std::string_view capabilityOutcomeName(CapabilityOutcome value) noexcept {
    switch (value) {
        case CapabilityOutcome::NotRequired: return "not-required";
        case CapabilityOutcome::Success: return "success";
        case CapabilityOutcome::Unsupported: return "unsupported";
        case CapabilityOutcome::Failed: return "failed";
        case CapabilityOutcome::MissingEvidence: return "missing-evidence";
    }
    return "unknown";
}

std::string_view sessionFinalStateName(SessionFinalState value) noexcept {
    switch (value) {
        case SessionFinalState::Running: return "running";
        case SessionFinalState::ReturnedToWindows: return "returned-to-windows";
        case SessionFinalState::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view sessionMetricsResultName(SessionMetricsResult value) noexcept {
    switch (value) {
        case SessionMetricsResult::Success: return "success";
        case SessionMetricsResult::UnsupportedSchema: return "unsupported-schema";
        case SessionMetricsResult::InvalidPlanFingerprint: return "invalid-plan-fingerprint";
        case SessionMetricsResult::InvalidSeatCount: return "invalid-seat-count";
        case SessionMetricsResult::InvalidSeatId: return "invalid-seat-id";
        case SessionMetricsResult::DuplicateSeatId: return "duplicate-seat-id";
        case SessionMetricsResult::InvalidInputMetrics: return "invalid-input-metrics";
        case SessionMetricsResult::InvalidRollbackState: return "invalid-rollback-state";
    }
    return "unknown";
}

} // namespace hydra::metrics
