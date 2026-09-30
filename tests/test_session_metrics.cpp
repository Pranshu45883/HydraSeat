#include "hydra/session_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::metrics;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

InputMetricSample sample(std::uint64_t correlation,
                         InputMetricStage stage,
                         std::uint64_t timestamp,
                         std::uint32_t expectedSeat,
                         std::uint32_t expectedProcess) {
    InputMetricSample value;
    value.correlationId = correlation;
    value.stage = stage;
    value.timestampMicros = timestamp;
    value.expectedSeatId = expectedSeat;
    value.targetProcessId = expectedProcess;
    value.eventClass = InputMetricEventClass::Key;
    if (stage == InputMetricStage::TargetApplied ||
        stage == InputMetricStage::TargetQueried) {
        value.receivingSeatId = expectedSeat;
        value.receivingProcessId = expectedProcess;
    }
    return value;
}

void appendCompleteEvent(InputMetricsSnapshot& snapshot,
                         std::uint64_t correlation,
                         std::uint64_t base,
                         std::uint32_t seat,
                         std::uint32_t process) {
    snapshot.samples.push_back(sample(correlation, InputMetricStage::PhysicalObserved,
                                      base, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteEnqueued,
                                      base + 10u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteDequeued,
                                      base + 20u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteWritten,
                                      base + 30u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::TargetApplied,
                                      base + 40u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::TargetQueried,
                                      base + 60u, seat, process));
}

SeatSessionMetrics seat(SeatId seatId, std::uint64_t base) {
    SeatSessionMetrics result;
    result.seatId = seatId;
    result.launchDurationMicros = base + 100u;
    result.stopDurationMicros = base + 200u;
    result.rollbackDurationMicros = base + 300u;
    result.processStarted = true;
    result.windowOwnershipVerified = true;
    result.displayPlacementVerified = true;
    result.inputRouteReady = true;
    result.controller = CapabilityOutcome::Success;
    result.audio = CapabilityOutcome::Success;
    return result;
}

SessionMetricsBuildInput complete(EvidenceOrigin origin) {
    SessionMetricsBuildInput input;
    input.planFingerprint = 0x12345678u;
    input.origin = origin;
    appendCompleteEvent(input.input, 1u, 1000u, 1u, 101u);
    appendCompleteEvent(input.input, 2u, 2000u, 2u, 202u);
    InputMetricSample resource;
    resource.stage = InputMetricStage::HostResourceSample;
    resource.timestampMicros = 3000u;
    resource.hostCpuTimeMicros = 4321u;
    resource.hostWorkingSetBytes = 96u * 1024u * 1024u;
    input.input.samples.push_back(resource);
    input.seats = {seat(2, 2000u), seat(1, 1000u)};
    input.finalState = SessionFinalState::Running;
    return input;
}

SessionMetricsBuildInput completeSingleSeat(EvidenceOrigin origin) {
    SessionMetricsBuildInput input;
    input.planFingerprint = 0x23456789u;
    input.origin = origin;
    appendCompleteEvent(input.input, 1u, 1000u, 1u, 101u);
    input.seats = {seat(1u, 1000u)};
    input.finalState = SessionFinalState::Running;
    return input;
}

void testControlledPassIsNotPhysicalValidation() {
    const auto input = complete(EvidenceOrigin::ControlledProcess);
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success,
          "complete controlled session builds an integrated report");
    check(report.isolationVerdict == EvidenceVerdict::Pass &&
              report.sessionVerdict == EvidenceVerdict::Pass,
          "loss-free receiver-complete controlled evidence can pass its declared controlled criteria");
    check(!report.physicalValidationEligible,
          "controlled process pass never masquerades as physical product validation");
    check(report.receiverEvidenceComplete && report.lossFreeEvidence &&
              report.input.receiverVerifiedEvents == 2u &&
              report.input.crossSeatEvents == 0u &&
              report.input.crossProcessEvents == 0u,
          "integrated report preserves receiver evidence and zero-cross counters");
    check(report.seats.size() == 2 && report.seats[0].seatId == 1 &&
              report.seats[1].seatId == 2 &&
              report.maximumLaunchDurationMicros == 2100u &&
              report.maximumRollbackDurationMicros == 2300u,
          "Seat metrics are canonicalized and aggregate timing maxima are deterministic");
    check(report.input.resourceSamples == 1u &&
              report.input.lastHostCpuTimeMicros == 4321u &&
              report.input.maxHostWorkingSetBytes == 96u * 1024u * 1024u,
          "bounded host CPU/memory samples survive the integrated report");

    const auto json = encodeSessionMetricsReportJson(report);
    check(json.find("\"evidence_origin\":\"controlled-process\"") != std::string::npos &&
              json.find("\"isolation_verdict\":\"pass\"") != std::string::npos &&
              json.find("\"receiver_evidence_complete\":true") != std::string::npos &&
              json.find("\"seat_id\":1") != std::string::npos,
          "machine-readable JSON exposes stable evidence/verdict/Seat fields");
    check(json.find("player") == std::string::npos &&
              json.find("path") == std::string::npos &&
              json.find("credential") == std::string::npos,
          "integrated aggregate JSON has no Player identity, path, or credential fields");
}

void testPhysicalPassIsExplicitlyEligible() {
    auto input = complete(EvidenceOrigin::Physical);
    input.finalState = SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = true;
    input.rollbackVerified = true;
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.sessionVerdict == EvidenceVerdict::Pass &&
              report.physicalValidationEligible,
          "only complete physical evidence can become physical-validation eligible");
}

void testSingleSeatPhysicalPassIsExplicitlyEligible() {
    auto input = completeSingleSeat(EvidenceOrigin::Physical);
    input.finalState = SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = true;
    input.rollbackVerified = true;
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.seats.size() == 1u && report.seats.front().seatId == 1u &&
              report.isolationVerdict == EvidenceVerdict::Pass &&
              report.sessionVerdict == EvidenceVerdict::Pass &&
              report.physicalValidationEligible,
          "one complete physical Seat can produce eligible evidence without inventing a second Seat");
}

void testMissingReceiverEvidenceNeverBecomesZeroBleedPass() {
    auto input = complete(EvidenceOrigin::Physical);
    for (auto& value : input.input.samples) {
        if (value.correlationId == 2u &&
            (value.stage == InputMetricStage::TargetApplied ||
             value.stage == InputMetricStage::TargetQueried)) {
            value.receivingSeatId = 0;
            value.receivingProcessId = 0;
        }
    }
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success,
          "missing receiver evidence remains reportable");
    check(report.input.crossSeatEvents == 0u &&
              report.input.crossProcessEvents == 0u &&
              !report.receiverEvidenceComplete &&
              report.isolationVerdict == EvidenceVerdict::InsufficientEvidence &&
              !report.physicalValidationEligible,
          "zero observed cross-events plus missing receiver evidence is insufficient, never a zero-bleed pass");
}

void testVerifiedCrossSeatEvidenceFails() {
    auto input = complete(EvidenceOrigin::Physical);
    for (auto& value : input.input.samples) {
        if (value.correlationId == 1u &&
            value.stage == InputMetricStage::TargetApplied) {
            value.receivingSeatId = 2u;
            value.receivingProcessId = 202u;
        }
    }
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.input.crossSeatEvents == 1u &&
              report.input.crossProcessEvents == 1u &&
              report.isolationVerdict == EvidenceVerdict::Fail &&
              report.sessionVerdict == EvidenceVerdict::Fail,
          "verified cross-Seat/process receiver evidence produces an explicit failure");
}

void testLossAndIncompleteCapabilityAreInsufficient() {
    auto input = complete(EvidenceOrigin::Physical);
    input.input.rotationDrops = 1u;
    input.seats[0].audio = CapabilityOutcome::Unsupported;
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              !report.lossFreeEvidence &&
              report.isolationVerdict == EvidenceVerdict::InsufficientEvidence &&
              report.sessionVerdict == EvidenceVerdict::InsufficientEvidence,
          "recorder loss and unsupported required route prevent a pass without fabricating failure/zero bleed");
}

void testExplicitFailuresAndRecoveryFailOverall() {
    auto input = complete(EvidenceOrigin::ControlledProcess);
    input.seats[1].controller = CapabilityOutcome::Failed;
    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.sessionVerdict == EvidenceVerdict::Fail,
          "explicit controller failure fails the overall session evidence");

    input = complete(EvidenceOrigin::ControlledProcess);
    input.finalState = SessionFinalState::RecoveryRequired;
    input.rollbackAttempted = true;
    input.rollbackVerified = false;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.sessionVerdict == EvidenceVerdict::Fail,
          "RecoveryRequired or unverified attempted rollback fails overall evidence");

    input = complete(EvidenceOrigin::ControlledProcess);
    input.finalState = SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = false;
    input.rollbackVerified = false;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success &&
              report.sessionVerdict == EvidenceVerdict::Fail,
          "ReturnedToWindows cannot pass without an attempted and verified rollback postcondition");
}

void testInputValidationFailsClosed() {
    SessionMetricsReport report;
    auto input = complete(EvidenceOrigin::Synthetic);
    input.planFingerprint = 0;
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::InvalidPlanFingerprint,
          "zero plan fingerprint is rejected");

    input = complete(EvidenceOrigin::Synthetic);
    input.seats.clear();
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::InvalidSeatCount,
          "session evidence requires at least one Seat metric record");

    input = complete(EvidenceOrigin::Synthetic);
    input.seats.push_back(seat(3u, 3000u));
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::InvalidSeatCount,
          "session evidence remains bounded to at most two Seat metric records");

    input = complete(EvidenceOrigin::Synthetic);
    input.seats[1].seatId = input.seats[0].seatId;
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::DuplicateSeatId,
          "duplicate Seat evidence is rejected");

    input = complete(EvidenceOrigin::Synthetic);
    input.rollbackVerified = true;
    input.rollbackAttempted = false;
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::InvalidRollbackState,
          "rollback cannot be declared verified when no rollback was attempted");

    input = complete(EvidenceOrigin::Synthetic);
    input.input.samples.clear();
    check(buildSessionMetricsReport(input, report) ==
              SessionMetricsResult::InvalidInputMetrics,
          "empty input trace cannot become an integrated zero-bleed report");
}

} // namespace

int main() {
    testControlledPassIsNotPhysicalValidation();
    testPhysicalPassIsExplicitlyEligible();
    testSingleSeatPhysicalPassIsExplicitlyEligible();
    testMissingReceiverEvidenceNeverBecomesZeroBleedPass();
    testVerifiedCrossSeatEvidenceFails();
    testLossAndIncompleteCapabilityAreInsufficient();
    testExplicitFailuresAndRecoveryFailOverall();
    testInputValidationFailsClosed();

    if (failures != 0) {
        std::cerr << failures << " session metrics test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Session metrics tests passed.\n";
    return EXIT_SUCCESS;
}
