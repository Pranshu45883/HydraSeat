#include "hydra/recovery_process_attachment.hpp"
#include "hydra/rollback_registry.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace hydra::watchdog;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

SessionId session(std::uint8_t seed = 0u) {
    SessionId value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index + 1u);
    }
    return value;
}

RollbackPlanManifest samplePlan() {
    RollbackPlanManifest manifest;
    manifest.lease.sessionId = session();
    manifest.lease.generation = 7;
    manifest.lease.timeoutMilliseconds = 500;
    manifest.rollbackTimeoutMilliseconds = 2'000;

    RollbackActionDescriptor process;
    process.actionId = 10;
    process.kind = RollbackActionKind::TerminateOwnedProcess;
    process.activationOrdinal = 1;
    process.timeoutMilliseconds = 500;
    process.generation = 1;
    process.process = {1234, 5678};

    RollbackActionDescriptor overlay;
    overlay.actionId = 20;
    overlay.kind = RollbackActionKind::ReleaseOverlayState;
    overlay.activationOrdinal = 3;
    overlay.timeoutMilliseconds = 250;
    overlay.generation = 2;
    overlay.resourceId = 0x1001;

    RollbackActionDescriptor backend;
    backend.actionId = 30;
    backend.kind = RollbackActionKind::ClearOptionalBackendState;
    backend.activationOrdinal = 2;
    backend.timeoutMilliseconds = 250;
    backend.generation = 3;
    backend.resourceId = 0x2001;

    manifest.actions = {process, overlay, backend};
    return manifest;
}

hydra::runtime::RuntimeSessionId runtimeSession(std::uint8_t seed) {
    hydra::runtime::RuntimeSessionId value;
    for (std::size_t index = 0; index < value.bytes.size(); ++index) {
        value.bytes[index] = static_cast<std::uint8_t>(seed + index + 1u);
    }
    return value;
}

hydra::recovery::RecoveryProcessAttachmentIdentity attachmentIdentity(
    hydra::SeatId seatId = 1u,
    ProcessIdentity process = {7001u, 0x1111222233334444ull},
    std::uint64_t sessionGeneration = 10u,
    std::uint64_t seatGameGeneration = 20u,
    std::uint64_t recoveryEpoch = 30u,
    std::uint8_t hostSessionSeed = 0x40u) {
    hydra::recovery::RecoveryProcessAttachmentIdentity identity;
    identity.seatId = seatId;
    identity.hostSessionId = runtimeSession(hostSessionSeed);
    identity.sessionGeneration = sessionGeneration;
    identity.seatGameGeneration = seatGameGeneration;
    identity.process = process;
    identity.recoveryEpoch = recoveryEpoch;
    return identity;
}

hydra::recovery::RecoveryProcessAttachmentRegistration attachmentRegistration(
    const hydra::recovery::RecoveryProcessAttachmentIdentity& identity,
    std::uint8_t leaseSessionSeed = 0x70u) {
    hydra::recovery::RecoveryProcessAttachmentRegistration registration;
    registration.identity = identity;
    registration.manifest.lease.sessionId = session(leaseSessionSeed);
    registration.manifest.lease.generation = identity.recoveryEpoch;
    registration.manifest.lease.timeoutMilliseconds = 1'000u;
    registration.manifest.rollbackTimeoutMilliseconds = 4'000u;

    RollbackActionDescriptor process;
    process.actionId = 100u;
    process.kind = RollbackActionKind::TerminateOwnedProcess;
    process.activationOrdinal = 1u;
    process.timeoutMilliseconds = 1'000u;
    process.generation = identity.recoveryEpoch;
    process.process = identity.process;

    RollbackActionDescriptor snapshot;
    snapshot.actionId = 200u;
    snapshot.kind = RollbackActionKind::RestoreSnapshotState;
    snapshot.activationOrdinal = 2u;
    snapshot.timeoutMilliseconds = 1'000u;
    snapshot.generation = identity.recoveryEpoch;
    snapshot.resourceId = 0xabc000ull + identity.seatId;

    registration.manifest.actions = {process, snapshot};
    return registration;
}

class FakeExecutor final : public RollbackExecutor {
public:
    std::vector<std::uint32_t> calls;
    std::vector<std::uint32_t> timeouts;
    std::unordered_map<std::uint32_t, RollbackActionResult> results;

    RollbackActionOutcome terminateOwnedProcess(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    RollbackActionOutcome closeOwnedSession(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    RollbackActionOutcome clearOptionalBackendState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    RollbackActionOutcome releaseOverlayState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    RollbackActionOutcome restoreSnapshotState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    RollbackActionOutcome writeSafeModeResult(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }

private:
    RollbackActionOutcome run(const RollbackActionDescriptor& action,
                              std::uint32_t timeoutMilliseconds) {
        calls.push_back(action.actionId);
        timeouts.push_back(timeoutMilliseconds);
        const auto found = results.find(action.actionId);
        const auto result = found == results.end()
            ? RollbackActionResult::Success
            : found->second;
        return {action.actionId, action.kind, result,
                result == RollbackActionResult::Failed ? 55u : 0u};
    }
};

void testPlanRoundTripAndBounds() {
    const auto manifest = samplePlan();
    std::string error;
    check(validateRollbackPlan(manifest, &error), "sample rollback plan validates");

    const auto encoded = encodeRegisterPlan(1, manifest);
    check(!encoded.empty() && encoded.size() <= kWatchdogMaxFrameBytes,
          "register-plan frame is bounded");
    const auto frame = decodeWatchdogFrame(encoded);
    check(frame && frame.frame->type == WatchdogMessageType::RegisterPlan &&
              frame.frame->sequence == 1,
          "register-plan frame header round trips");

    RollbackPlanManifest decoded;
    check(decodeRegisterPlan(*frame.frame, decoded, &error) && decoded == manifest,
          "register-plan payload round trips exactly");
}

void testMalformedFramesFailClosed() {
    const auto manifest = samplePlan();
    auto encoded = encodeRegisterPlan(4, manifest);
    check(!encoded.empty(), "baseline frame encodes");

    auto badMagic = encoded;
    badMagic[0] = std::byte{0};
    check(!decodeWatchdogFrame(badMagic), "bad magic is rejected");

    auto badVersion = encoded;
    badVersion[4] = std::byte{2};
    check(!decodeWatchdogFrame(badVersion), "future protocol version is rejected");

    auto badReserved = encoded;
    badReserved[12] = std::byte{1};
    check(!decodeWatchdogFrame(badReserved), "nonzero frame reserved field is rejected");

    auto truncated = encoded;
    truncated.pop_back();
    check(!decodeWatchdogFrame(truncated), "truncated payload is rejected");

    auto oversizedPayload = std::vector<std::byte>(kWatchdogMaxPayloadBytes + 1);
    check(encodeWatchdogFrame(WatchdogMessageType::Status, 1,
                              oversizedPayload).empty(),
          "oversized payload cannot be encoded");
}

void testManifestValidationRejectsAmbiguity() {
    std::string error;

    auto zeroSession = samplePlan();
    zeroSession.lease.sessionId = {};
    check(!validateRollbackPlan(zeroSession, &error),
          "zero session id is rejected");

    auto duplicateId = samplePlan();
    duplicateId.actions[1].actionId = duplicateId.actions[0].actionId;
    check(!validateRollbackPlan(duplicateId, &error),
          "duplicate action id is rejected");

    auto duplicateOrdinal = samplePlan();
    duplicateOrdinal.actions[1].activationOrdinal =
        duplicateOrdinal.actions[0].activationOrdinal;
    check(!validateRollbackPlan(duplicateOrdinal, &error),
          "duplicate activation ordinal is rejected");

    auto unknown = samplePlan();
    unknown.actions[0].kind = static_cast<RollbackActionKind>(999);
    check(!validateRollbackPlan(unknown, &error),
          "unknown rollback action kind is rejected");

    auto reusedPid = samplePlan();
    reusedPid.actions[0].process.creationTime100ns = 0;
    check(!validateRollbackPlan(reusedPid, &error),
          "process action requires creation identity");

    auto resourceWithPid = samplePlan();
    resourceWithPid.actions[1].process = {42, 99};
    check(!validateRollbackPlan(resourceWithPid, &error),
          "resource action cannot smuggle a process identity");

    auto tooMany = samplePlan();
    tooMany.actions.clear();
    for (std::size_t index = 0; index < kWatchdogMaxRollbackActions + 1; ++index) {
        RollbackActionDescriptor action;
        action.actionId = static_cast<std::uint32_t>(index + 1);
        action.kind = RollbackActionKind::ReleaseOverlayState;
        action.activationOrdinal = static_cast<std::uint32_t>(index + 1);
        action.timeoutMilliseconds = 100;
        action.generation = 1;
        action.resourceId = static_cast<std::uint64_t>(index + 1);
        tooMany.actions.push_back(action);
    }
    check(!validateRollbackPlan(tooMany, &error),
          "action count above hard maximum is rejected");
}

void testLeaseAndStatusRoundTrip() {
    const WatchdogLease lease{session(), 77, 750};
    auto frame = decodeWatchdogFrame(encodeLeaseRenewal(9, lease));
    WatchdogLease decodedLease;
    std::string error;
    check(frame && decodeLeaseRenewal(*frame.frame, decodedLease, &error) &&
              decodedLease == lease,
          "lease renewal round trips");

    frame = decodeWatchdogFrame(encodeDisarm(10, lease));
    check(frame && decodeDisarm(*frame.frame, decodedLease, &error) &&
              decodedLease == lease,
          "disarm round trips");

    WatchdogStatus status;
    status.sessionId = session();
    status.generation = 77;
    status.state = WatchdogRunState::RecoveryRequired;
    status.reason = WatchdogTriggerReason::HostExited;
    status.completedActions = 2;
    status.totalActions = 3;
    status.failedActionId = 30;
    status.systemError = 5;
    frame = decodeWatchdogFrame(encodeWatchdogStatus(11, status));
    WatchdogStatus decodedStatus;
    check(frame && decodeWatchdogStatus(*frame.frame, decodedStatus, &error) &&
              decodedStatus == status,
          "watchdog status round trips");
}

void testRegistryReverseOrderAndIdempotency() {
    RollbackRegistry registry;
    const auto manifest = samplePlan();
    std::string error;
    check(registry.registerPlan(manifest, &error), "registry accepts valid plan");
    check(registry.registerPlan(manifest, &error),
          "registering identical plan is idempotent");

    FakeExecutor executor;
    const auto first = registry.execute(executor);
    check(first.allSatisfied && !first.recoveryRequired,
          "successful rollback satisfies the plan");
    check(executor.calls == std::vector<std::uint32_t>{20, 30, 10},
          "rollback executes reverse activation order");

    executor.calls.clear();
    executor.timeouts.clear();
    const auto second = registry.execute(executor);
    check(second.allSatisfied && executor.calls.empty(),
          "second rollback performs no already-completed mutation");
    check(second.outcomes.size() == manifest.actions.size(),
          "idempotent replay still reports every action");
    for (const auto& outcome : second.outcomes) {
        check(outcome.result == RollbackActionResult::AlreadySatisfied,
              "idempotent replay reports already-satisfied");
    }
}

void testPartialFailureContinuesAndCanRetry() {
    RollbackRegistry registry;
    const auto manifest = samplePlan();
    check(registry.registerPlan(manifest), "partial-failure plan registers");

    FakeExecutor executor;
    executor.results[30] = RollbackActionResult::Failed;
    const auto first = registry.execute(executor);
    check(first.recoveryRequired && !first.allSatisfied &&
              first.firstFailedActionId == 30 && first.firstSystemError == 55,
          "partial failure enters recovery-required with exact action");
    check(executor.calls == std::vector<std::uint32_t>{20, 30, 10},
          "partial failure does not prevent later independent cleanup");

    executor.calls.clear();
    executor.results[30] = RollbackActionResult::Success;
    const auto retry = registry.execute(executor);
    check(retry.allSatisfied && !retry.recoveryRequired,
          "failed action can be retried to completion");
    check(executor.calls == std::vector<std::uint32_t>{30},
          "retry invokes only the previously unsatisfied action");
}

void testDifferentPlanCannotReplaceArmedRegistry() {
    RollbackRegistry registry;
    auto first = samplePlan();
    auto second = first;
    second.lease.generation += 1;
    std::string error;
    check(registry.registerPlan(first, &error), "first plan registers");
    check(!registry.registerPlan(second, &error),
          "different plan cannot replace armed registry");
    registry.clear();
    check(registry.registerPlan(second, &error),
          "explicit clear permits a new generation");
}

void testRestartedRegistryCanTreatExternalCleanupAsSatisfied() {
    const auto manifest = samplePlan();
    RollbackRegistry restarted;
    check(restarted.registerPlan(manifest),
          "reconstructed registry accepts the same bounded manifest");
    FakeExecutor executor;
    executor.results[20] = RollbackActionResult::AlreadySatisfied;
    executor.results[30] = RollbackActionResult::AlreadySatisfied;
    executor.results[10] = RollbackActionResult::AlreadySatisfied;
    const auto result = restarted.execute(executor);
    check(result.allSatisfied && !result.recoveryRequired,
          "reloaded plan is safe when prior rollback already completed");
}

void testRecoveryAttachmentIdentityCodecAndPlanBinding() {
    using namespace hydra::recovery;

    const auto identity = attachmentIdentity();
    std::string error;
    check(validateRecoveryProcessAttachmentIdentity(identity, &error),
          "exact recovery attachment identity validates");
    const auto encoded = encodeRecoveryProcessAttachmentIdentity(identity);
    check(encoded.size() == kRecoveryProcessAttachmentIdentityBytes,
          "recovery attachment identity uses its exact fixed-width encoding");
    check(encodeRecoveryProcessAttachmentIdentity(identity) == encoded,
          "recovery attachment identity encoding is deterministic");
    const auto decoded = decodeRecoveryProcessAttachmentIdentity(encoded, &error);
    check(decoded && *decoded == identity,
          "recovery attachment identity round trips exactly");

    auto badVersion = encoded;
    badVersion[4] = std::byte{2};
    check(!decodeRecoveryProcessAttachmentIdentity(badVersion, &error),
          "future recovery attachment identity version is rejected");
    auto badReserved = encoded;
    badReserved[6] = std::byte{1};
    check(!decodeRecoveryProcessAttachmentIdentity(badReserved, &error),
          "nonzero recovery attachment reserved field is rejected");
    auto truncated = encoded;
    truncated.pop_back();
    check(!decodeRecoveryProcessAttachmentIdentity(truncated, &error),
          "truncated recovery attachment identity is rejected");

    auto noCreation = identity;
    noCreation.process.creationTime100ns = 0u;
    check(!validateRecoveryProcessAttachmentIdentity(noCreation, &error),
          "PID without creation time cannot authorize recovery attachment");
    auto thirdSeat = identity;
    thirdSeat.seatId = 3u;
    check(!validateRecoveryProcessAttachmentIdentity(thirdSeat, &error),
          "third Seat cannot enter the v1 recovery attachment authority");

    const auto registration = attachmentRegistration(identity);
    check(validateRecoveryProcessAttachmentRegistration(registration, &error),
          "exact process-bound recovery plan validates");
    auto escapedProcess = registration;
    escapedProcess.manifest.actions[0].process = {9999u, 0x9999000011112222ull};
    check(!validateRecoveryProcessAttachmentRegistration(escapedProcess, &error),
          "attachment registration cannot redirect its process rollback action");
    auto extraProcess = registration;
    auto secondProcess = extraProcess.manifest.actions[0];
    secondProcess.actionId = 300u;
    secondProcess.activationOrdinal = 3u;
    extraProcess.manifest.actions.push_back(secondProcess);
    check(!validateRecoveryProcessAttachmentRegistration(extraProcess, &error),
          "attachment registration cannot smuggle a second process target");
}

void testRecoveryAttachmentAuthorityExactEpochsAndDisarm() {
    using namespace hydra::recovery;

    RecoveryProcessAttachmentAuthority authority;
    const auto firstIdentity = attachmentIdentity();
    const auto first = attachmentRegistration(firstIdentity, 0x70u);
    auto result = authority.registerAttachment(first);
    check(result.code == RecoveryAttachmentCode::Ok && result.succeeded(),
          "exact recovery attachment registers");
    check(authority.registerAttachment(first).code ==
              RecoveryAttachmentCode::AlreadySatisfied,
          "exact duplicate recovery registration is idempotent");

    auto conflicting = first;
    ++conflicting.manifest.actions[1].resourceId;
    check(authority.registerAttachment(conflicting).code ==
              RecoveryAttachmentCode::ConflictingRegistration,
          "same exact identity cannot replace an armed manifest");

    auto reusedPidIdentity = firstIdentity;
    ++reusedPidIdentity.process.creationTime100ns;
    const auto reusedPid = attachmentRegistration(reusedPidIdentity, 0x72u);
    check(authority.registerAttachment(reusedPid).code ==
              RecoveryAttachmentCode::ProcessIdentityMismatch,
          "same Seat PID with a different creation time cannot replace an active attachment");

    auto wrongSeatIdentity = firstIdentity;
    wrongSeatIdentity.seatId = 2u;
    const auto wrongSeat = attachmentRegistration(wrongSeatIdentity, 0x73u);
    check(authority.registerAttachment(wrongSeat).code ==
              RecoveryAttachmentCode::SeatMismatch,
          "another Seat cannot adopt an already attached exact process");

    auto wrongSession = firstIdentity;
    wrongSession.hostSessionId = runtimeSession(0x55u);
    check(authority.verifyArmed(wrongSession, first.manifest.lease).code ==
              RecoveryAttachmentCode::SessionMismatch,
          "wrong host session cannot verify an attachment");
    auto staleSession = firstIdentity;
    --staleSession.sessionGeneration;
    check(authority.verifyArmed(staleSession, first.manifest.lease).code ==
              RecoveryAttachmentCode::StaleSessionGeneration,
          "stale host session generation cannot verify an attachment");
    auto staleGame = firstIdentity;
    --staleGame.seatGameGeneration;
    check(authority.verifyArmed(staleGame, first.manifest.lease).code ==
              RecoveryAttachmentCode::StaleSeatGameGeneration,
          "stale Seat-game generation cannot verify an attachment");
    auto wrongLease = first.manifest.lease;
    ++wrongLease.generation;
    check(authority.verifyArmed(firstIdentity, wrongLease).code ==
              RecoveryAttachmentCode::LeaseMismatch,
          "wrong watchdog lease cannot verify an attachment");

    auto duplicateLeaseIdentity = attachmentIdentity(
        2u, {7002u, 0x5555666677778888ull}, 10u, 20u,
        firstIdentity.recoveryEpoch, 0x40u);
    auto duplicateLease = attachmentRegistration(duplicateLeaseIdentity, 0x74u);
    duplicateLease.manifest.lease.sessionId = first.manifest.lease.sessionId;
    ++duplicateLease.manifest.lease.timeoutMilliseconds;
    check(authority.registerAttachment(duplicateLease).code ==
              RecoveryAttachmentCode::LeaseMismatch,
          "another Seat cannot reuse the same watchdog lease identity with altered timeout");

    const auto secondIdentity = attachmentIdentity(
        2u, {7002u, 0x5555666677778888ull}, 10u, 20u, 31u, 0x40u);
    const auto second = attachmentRegistration(secondIdentity, 0x74u);
    check(authority.registerAttachment(second).code == RecoveryAttachmentCode::Ok,
          "Seat 2 owns an independent exact recovery attachment");
    const auto active = authority.activeAttachments();
    check(active.size() == 2u && active[0].identity.seatId == 1u &&
              active[1].identity.seatId == 2u,
          "two active attachments are bounded and deterministically ordered by Seat");

    check(authority.disarm(firstIdentity, first.manifest.lease).code ==
              RecoveryAttachmentCode::Ok,
          "exact identity and lease disarm Seat 1 recovery ownership");
    check(authority.verifyArmed(secondIdentity, second.manifest.lease).succeeded(),
          "Seat 1 disarm does not affect Seat 2 recovery ownership");
    check(authority.disarm(firstIdentity, first.manifest.lease).code ==
              RecoveryAttachmentCode::AlreadySatisfied,
          "repeated exact disarm is idempotent");
    check(authority.registerAttachment(first).code ==
              RecoveryAttachmentCode::ReplayRejected,
          "completed exact attachment cannot be replayed as new authority");

    auto staleEpochIdentity = firstIdentity;
    ++staleEpochIdentity.seatGameGeneration;
    staleEpochIdentity.process.creationTime100ns += 50u;
    --staleEpochIdentity.recoveryEpoch;
    const auto staleEpoch = attachmentRegistration(staleEpochIdentity, 0x76u);
    check(authority.registerAttachment(staleEpoch).code ==
              RecoveryAttachmentCode::ReplayRejected,
          "newer Seat-game generation cannot revive a completed stale recovery epoch");

    check(authority.disarm(secondIdentity, second.manifest.lease).succeeded(),
          "exact Seat 2 attachment disarms before completed-lease replay audit");
    auto migratedLeaseIdentity = secondIdentity;
    ++migratedLeaseIdentity.seatGameGeneration;
    migratedLeaseIdentity.process.creationTime100ns += 100u;
    migratedLeaseIdentity.recoveryEpoch = firstIdentity.recoveryEpoch;
    auto migratedLease = attachmentRegistration(migratedLeaseIdentity, 0x77u);
    migratedLease.manifest.lease.sessionId = first.manifest.lease.sessionId;
    ++migratedLease.manifest.lease.timeoutMilliseconds;
    const auto migratedLeaseResult = authority.registerAttachment(migratedLease);
    check(migratedLeaseResult.code == RecoveryAttachmentCode::ReplayRejected &&
              migratedLeaseResult.diagnostic.find("completed watchdog lease identity") !=
                  std::string::npos,
          "completed watchdog lease identity cannot migrate to another Seat with altered timeout");

    auto nextIdentity = firstIdentity;
    ++nextIdentity.seatGameGeneration;
    nextIdentity.process.creationTime100ns += 100u;
    ++nextIdentity.recoveryEpoch;
    const auto next = attachmentRegistration(nextIdentity, 0x75u);
    check(authority.registerAttachment(next).code == RecoveryAttachmentCode::Ok,
          "newer Seat-game epoch may attach a reused PID with a new creation identity");
    check(authority.disarm(firstIdentity, first.manifest.lease).code ==
              RecoveryAttachmentCode::StaleSeatGameGeneration,
          "stale prior process identity cannot disarm the newer Seat epoch");
    check(authority.verifyArmed(nextIdentity, next.manifest.lease).succeeded(),
          "new exact attachment remains armed after stale disarm attempt");

    check(authority.disarm(nextIdentity, next.manifest.lease).succeeded(),
          "new exact Seat 1 attachment disarms cleanly");
    check(authority.disarm(secondIdentity, second.manifest.lease).code ==
              RecoveryAttachmentCode::AlreadySatisfied,
          "completed exact Seat 2 disarm remains idempotent");
    check(authority.activeAttachments().empty(),
          "no recovery attachment remains after both exact disarms");
}

} // namespace

int main() {
    testPlanRoundTripAndBounds();
    testMalformedFramesFailClosed();
    testManifestValidationRejectsAmbiguity();
    testLeaseAndStatusRoundTrip();
    testRegistryReverseOrderAndIdempotency();
    testPartialFailureContinuesAndCanRetry();
    testDifferentPlanCannotReplaceArmedRegistry();
    testRestartedRegistryCanTreatExternalCleanupAsSatisfied();
    testRecoveryAttachmentIdentityCodecAndPlanBinding();
    testRecoveryAttachmentAuthorityExactEpochsAndDisarm();

    std::cout << "Watchdog protocol/registry tests passed.\n";
    return EXIT_SUCCESS;
}
