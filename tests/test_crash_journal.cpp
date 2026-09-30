#include "hydra/crash_journal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace hydra::recovery;
using namespace hydra::watchdog;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

SessionId session(std::uint8_t seed = 0x10) {
    SessionId value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index + 1);
    }
    return value;
}

Hash256 hash(std::uint8_t seed) {
    Hash256 value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index + 1);
    }
    return value;
}

RollbackPlanManifest samplePlan(std::uint8_t seed = 0x10) {
    RollbackPlanManifest manifest;
    manifest.lease.sessionId = session(seed);
    manifest.lease.generation = 9;
    manifest.lease.timeoutMilliseconds = 500;
    manifest.rollbackTimeoutMilliseconds = 2'000;

    RollbackActionDescriptor process;
    process.actionId = 10;
    process.kind = RollbackActionKind::TerminateOwnedProcess;
    process.activationOrdinal = 1;
    process.timeoutMilliseconds = 500;
    process.generation = 2;
    process.process = {1234, 0x1020304050607080ull};

    RollbackActionDescriptor overlay;
    overlay.actionId = 20;
    overlay.kind = RollbackActionKind::ReleaseOverlayState;
    overlay.activationOrdinal = 2;
    overlay.timeoutMilliseconds = 500;
    overlay.generation = 3;
    overlay.resourceId = 0x2001;

    manifest.actions = {process, overlay};
    return manifest;
}

RecoveryProcessAttachmentIdentity attachmentIdentity() {
    RecoveryProcessAttachmentIdentity identity;
    identity.seatId = 1u;
    for (std::size_t index = 0; index < identity.hostSessionId.bytes.size(); ++index) {
        identity.hostSessionId.bytes[index] =
            static_cast<std::uint8_t>(0x80u + index + 1u);
    }
    identity.sessionGeneration = 7u;
    identity.seatGameGeneration = 11u;
    identity.process = samplePlan().actions.front().process;
    identity.recoveryEpoch = samplePlan().lease.generation;
    return identity;
}

std::vector<SnapshotReference> snapshots() {
    return {
        {1001, hash(0x40), 4},
        {1002, hash(0x60), 5},
    };
}

CrashJournalState initialState(const RollbackPlanManifest& manifest,
                               std::uint64_t runtimeGeneration = 7) {
    std::string error;
    const auto state = makeInitialCrashJournal(
        manifest, runtimeGeneration, snapshots(), &error);
    check(state.has_value(), "initial crash journal is constructible");
    return *state;
}

void append(CrashJournalState& state,
            const RollbackPlanManifest& manifest,
            CrashJournalRecordKind kind,
            std::uint32_t actionId,
            std::uint64_t generation) {
    std::string error;
    check(appendCrashJournalRecord(
              state, manifest, kind, actionId, generation, &error),
          "expected journal transition appends");
}

CrashJournalState cleanState(const RollbackPlanManifest& manifest,
                             std::uint64_t runtimeGeneration = 7) {
    auto state = initialState(manifest, runtimeGeneration);
    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2);
    append(state, manifest, CrashJournalRecordKind::ActionApplied, 10, 2);
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 10, 2);
    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionApplied, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActivationCommitted,
           0, runtimeGeneration);
    append(state, manifest, CrashJournalRecordKind::RollbackStarted,
           0, runtimeGeneration);
    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 10, 2);
    append(state, manifest, CrashJournalRecordKind::RollbackVerified,
           0, runtimeGeneration);
    append(state, manifest, CrashJournalRecordKind::CleanStop,
           0, runtimeGeneration);
    return state;
}

class FakeStorage final : public CrashJournalStorage {
public:
    std::map<JournalStorageSlot, std::vector<std::byte>> files;
    std::map<JournalStorageSlot, JournalReadStatus> forcedReads;
    std::optional<JournalStorageSlot> failWriteOnce;
    std::optional<std::pair<JournalStorageSlot, JournalStorageSlot>>
        failReplaceOnce;
    std::optional<JournalStorageSlot> failRemoveOnce;
    std::size_t durableWrites{0};
    std::size_t atomicReplaces{0};

    JournalReadResult read(JournalStorageSlot slot,
                           std::size_t maxBytes) override {
        if (const auto forced = forcedReads.find(slot);
            forced != forcedReads.end()) {
            return {forced->second, {}, 123};
        }
        const auto found = files.find(slot);
        if (found == files.end()) {
            return {JournalReadStatus::Missing, {}, 0};
        }
        if (found->second.size() > maxBytes) {
            return {JournalReadStatus::TooLarge, {}, 0};
        }
        return {JournalReadStatus::Success, found->second, 0};
    }

    bool durableWrite(JournalStorageSlot slot,
                      std::span<const std::byte> bytes,
                      std::uint32_t* systemError) override {
        ++durableWrites;
        if (failWriteOnce.has_value() && *failWriteOnce == slot) {
            failWriteOnce.reset();
            if (systemError != nullptr) *systemError = 112;
            return false;
        }
        files[slot] = std::vector<std::byte>(bytes.begin(), bytes.end());
        if (systemError != nullptr) *systemError = 0;
        return true;
    }

    bool atomicReplace(JournalStorageSlot from,
                       JournalStorageSlot to,
                       std::uint32_t* systemError) override {
        ++atomicReplaces;
        if (failReplaceOnce.has_value() &&
            *failReplaceOnce == std::pair{from, to}) {
            failReplaceOnce.reset();
            if (systemError != nullptr) *systemError = 5;
            return false;
        }
        const auto found = files.find(from);
        if (found == files.end()) {
            if (systemError != nullptr) *systemError = 2;
            return false;
        }
        files[to] = std::move(found->second);
        files.erase(found);
        if (systemError != nullptr) *systemError = 0;
        return true;
    }

    bool remove(JournalStorageSlot slot,
                std::uint32_t* systemError) override {
        if (failRemoveOnce.has_value() && *failRemoveOnce == slot) {
            failRemoveOnce.reset();
            if (systemError != nullptr) *systemError = 5;
            return false;
        }
        files.erase(slot);
        if (systemError != nullptr) *systemError = 0;
        return true;
    }
};

void placeCurrent(FakeStorage& storage, const CrashJournalState& state) {
    const auto bytes = encodeCrashJournal(state);
    check(!bytes.empty(), "fixture journal encodes");
    storage.files[JournalStorageSlot::Current] = bytes;
}

void testCodecRoundTripAndCorruption() {
    const auto manifest = samplePlan();
    const auto state = cleanState(manifest);
    const auto bytes = encodeCrashJournal(state);
    check(!bytes.empty() && bytes.size() <= kCrashJournalMaxFileBytes,
          "clean journal encodes within hard bound");
    const std::size_t expected = kCrashJournalHeaderBytes + 80 +
        state.records.size() * 24 + state.snapshots.size() * 48;
    check(bytes.size() == expected,
          "journal encoding is fixed-width and count-derived");

    std::string error;
    const auto decoded = decodeCrashJournal(bytes, &error);
    check(decoded.has_value() && *decoded == state,
          "crash journal round trips exactly");

    auto corrupted = bytes;
    corrupted.back() ^= std::byte{0x01};
    check(!decodeCrashJournal(corrupted, &error),
          "payload corruption is rejected by checksum");

    auto future = bytes;
    future[4] = std::byte{2};
    check(!decodeCrashJournal(future, &error),
          "future schema version fails closed");

    auto truncated = bytes;
    truncated.pop_back();
    check(!decodeCrashJournal(truncated, &error),
          "truncated journal fails closed");

    std::vector<std::byte> oversized(kCrashJournalMaxFileBytes + 1);
    check(!decodeCrashJournal(oversized, &error),
          "oversized journal fails before parsing");
}

void testSafeModeCodec() {
    SafeModeMarker marker;
    marker.sessionId = session();
    marker.runtimeGeneration = 7;
    marker.reason = SafeModeReason::RecoveryRequired;
    marker.diagnosticCode = 42;
    marker.journalHash = hash(0x80);

    const auto bytes = encodeSafeModeMarker(marker);
    check(bytes.size() == kSafeModeMarkerFileBytes,
          "safe-mode marker has exact bounded size");
    std::string error;
    const auto decoded = decodeSafeModeMarker(bytes, &error);
    check(decoded.has_value() && *decoded == marker,
          "safe-mode marker round trips exactly");

    auto corrupt = bytes;
    corrupt[20] ^= std::byte{0x80};
    check(!decodeSafeModeMarker(corrupt, &error),
          "safe-mode corruption fails closed");
}

void testCanonicalPlanHashBinding() {
    const auto manifest = samplePlan();
    const auto state = initialState(manifest);
    const auto canonicalHash = hashRollbackPlanManifest(manifest);
    check(!isZeroHash(canonicalHash),
          "valid rollback plan produces a nonzero canonical hash");
    check(state.planHash == canonicalHash,
          "initial journal derives its plan hash from the trusted manifest");
    check(hashRollbackPlanManifest(manifest) == canonicalHash,
          "rollback plan hashing is deterministic");

    auto changedManifest = manifest;
    changedManifest.rollbackTimeoutMilliseconds += 1;
    check(hashRollbackPlanManifest(changedManifest) != canonicalHash,
          "material rollback-plan changes alter the canonical hash");
    std::string error;
    check(!validateCrashJournalAgainstPlan(state, changedManifest, &error),
          "journal cannot be rebound to a different valid rollback plan");
}

void testRecoveryProcessAttachmentJournalBinding() {
    const auto manifest = samplePlan();
    const auto identity = attachmentIdentity();
    std::string error;
    const auto attachmentSnapshot =
        makeRecoveryProcessAttachmentSnapshot(identity, &error);
    check(attachmentSnapshot.has_value() &&
              attachmentSnapshot->snapshotId ==
                  kRecoveryProcessAttachmentSnapshotId &&
              attachmentSnapshot->generation == identity.recoveryEpoch,
          "exact recovery attachment produces a reserved bounded journal snapshot");
    const std::array boundSnapshots{*attachmentSnapshot};
    const auto state = makeInitialCrashJournal(
        manifest, 7u, boundSnapshots, &error);
    check(state.has_value() &&
              validateRecoveryProcessAttachmentJournalBinding(
                  *state, identity, &error),
          "journal binds the exact Seat/session/process recovery epoch");

    const auto encoded = encodeCrashJournal(*state);
    const auto decoded = decodeCrashJournal(encoded, &error);
    check(decoded &&
              validateRecoveryProcessAttachmentJournalBinding(
                  *decoded, identity, &error),
          "restart replay preserves the exact recovery attachment binding");

    auto wrongSeat = identity;
    wrongSeat.seatId = 2u;
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, wrongSeat, &error),
          "journal binding rejects another Seat");
    auto wrongSession = identity;
    ++wrongSession.hostSessionId.bytes[0];
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, wrongSession, &error),
          "journal binding rejects another host session");
    auto staleSessionGeneration = identity;
    --staleSessionGeneration.sessionGeneration;
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, staleSessionGeneration, &error),
          "journal binding rejects a stale session generation");
    auto staleSeatGameGeneration = identity;
    --staleSeatGameGeneration.seatGameGeneration;
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, staleSeatGameGeneration, &error),
          "journal binding rejects a stale Seat-game generation");
    auto reusedPid = identity;
    ++reusedPid.process.creationTime100ns;
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, reusedPid, &error),
          "journal binding rejects PID reuse with another creation time");
    auto wrongEpoch = identity;
    ++wrongEpoch.recoveryEpoch;
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *decoded, wrongEpoch, &error),
          "journal binding rejects another recovery epoch");

    const auto legacyState = initialState(manifest);
    const auto legacyBytes = encodeCrashJournal(legacyState);
    const auto legacyDecoded = decodeCrashJournal(legacyBytes, &error);
    check(legacyDecoded.has_value(),
          "existing schema-v1 journal without attachment metadata remains readable");
    check(!validateRecoveryProcessAttachmentJournalBinding(
              *legacyDecoded, identity, &error),
          "legacy journal without exact attachment binding cannot authorize production attachment recovery");
}

void testStateMachineAndPlanBinding() {
    const auto manifest = samplePlan();
    auto state = initialState(manifest);
    std::string error;

    auto invalid = state;
    check(!appendCrashJournalRecord(
              invalid, manifest, CrashJournalRecordKind::ActionApplied,
              10, 2, &error),
          "apply before prepare is rejected");
    check(invalid == state, "rejected append mutates nothing");

    check(!appendCrashJournalRecord(
              invalid, manifest, CrashJournalRecordKind::ActionPrepared,
              999, 2, &error),
          "unknown action id cannot enter the journal");
    check(!appendCrashJournalRecord(
              invalid, manifest, CrashJournalRecordKind::ActionPrepared,
              10, 999, &error),
          "wrong action generation cannot enter the journal");

    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2);
    check(!appendCrashJournalRecord(
              state, manifest, CrashJournalRecordKind::ActivationCommitted,
              0, 7, &error),
          "activation cannot commit before prepared action verification");

    append(state, manifest, CrashJournalRecordKind::ActionApplied, 10, 2);
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 10, 2);
    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionApplied, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActivationCommitted, 0, 7);
    append(state, manifest, CrashJournalRecordKind::RollbackStarted, 0, 7);

    auto wrongOrder = state;
    check(appendCrashJournalRecord(
              wrongOrder, manifest, CrashJournalRecordKind::ActionRolledBack,
              10, 2, &error),
          "first rollback marker can identify one outstanding action");
    check(!appendCrashJournalRecord(
              wrongOrder, manifest, CrashJournalRecordKind::ActionRolledBack,
              20, 3, &error),
          "rollback journal rejects forward activation order");

    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 20, 3);
    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 10, 2);
    append(state, manifest, CrashJournalRecordKind::RollbackVerified, 0, 7);
    append(state, manifest, CrashJournalRecordKind::CleanStop, 0, 7);
    check(state.phase == CrashJournalPhase::Clean &&
              state.finalResult == CrashJournalFinalResult::Clean,
          "verified reverse rollback can reach clean state");
    check(validateCrashJournalAgainstPlan(state, manifest, &error),
          "clean journal remains bound to exact rollback plan");
}

void testMaximumPlanFitsBoundedJournal() {
    RollbackPlanManifest manifest;
    manifest.lease.sessionId = session(0x30);
    manifest.lease.generation = 1;
    manifest.lease.timeoutMilliseconds = 500;
    manifest.rollbackTimeoutMilliseconds = 10'000;
    for (std::uint32_t index = 0;
         index < static_cast<std::uint32_t>(kWatchdogMaxRollbackActions);
         ++index) {
        RollbackActionDescriptor action;
        action.actionId = index + 1;
        action.kind = RollbackActionKind::ReleaseOverlayState;
        action.activationOrdinal = index + 1;
        action.timeoutMilliseconds = 100;
        action.generation = static_cast<std::uint64_t>(index + 1);
        action.resourceId = 0x1000ull + index;
        manifest.actions.push_back(action);
    }
    std::string error;
    auto state = makeInitialCrashJournal(
        manifest, 2, {}, &error);
    check(state.has_value(), "maximum action plan creates bounded journal");
    for (const auto& action : manifest.actions) {
        append(*state, manifest, CrashJournalRecordKind::ActionPrepared,
               action.actionId, action.generation);
        append(*state, manifest, CrashJournalRecordKind::ActionApplied,
               action.actionId, action.generation);
        append(*state, manifest, CrashJournalRecordKind::ActionVerified,
               action.actionId, action.generation);
    }
    append(*state, manifest, CrashJournalRecordKind::ActivationCommitted, 0, 2);
    append(*state, manifest, CrashJournalRecordKind::RollbackStarted, 0, 2);
    for (auto it = manifest.actions.rbegin(); it != manifest.actions.rend(); ++it) {
        append(*state, manifest, CrashJournalRecordKind::ActionRolledBack,
               it->actionId, it->generation);
    }
    append(*state, manifest, CrashJournalRecordKind::RollbackVerified, 0, 2);
    append(*state, manifest, CrashJournalRecordKind::CleanStop, 0, 2);
    check(state->records.size() == 133,
          "maximum 32-action full lifecycle needs 133 bounded records");
    check(state->records.size() <= kCrashJournalMaxRecords,
          "record ceiling contains maximum full lifecycle");
    check(encodeCrashJournal(*state).size() <= kCrashJournalMaxFileBytes,
          "maximum full lifecycle remains under file-size ceiling");
}

void testStartupAssessmentAtTransitionBoundaries() {
    const auto manifest = samplePlan();
    auto state = initialState(manifest);
    std::vector<CrashJournalState> boundaries{state};
    const auto capture = [&]() { boundaries.push_back(state); };

    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2); capture();
    append(state, manifest, CrashJournalRecordKind::ActionApplied, 10, 2); capture();
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 10, 2); capture();
    append(state, manifest, CrashJournalRecordKind::ActionPrepared, 20, 3); capture();
    append(state, manifest, CrashJournalRecordKind::ActionApplied, 20, 3); capture();
    append(state, manifest, CrashJournalRecordKind::ActionVerified, 20, 3); capture();
    append(state, manifest, CrashJournalRecordKind::ActivationCommitted, 0, 7); capture();
    append(state, manifest, CrashJournalRecordKind::RollbackStarted, 0, 7); capture();
    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 20, 3); capture();
    append(state, manifest, CrashJournalRecordKind::ActionRolledBack, 10, 2); capture();
    append(state, manifest, CrashJournalRecordKind::RollbackVerified, 0, 7); capture();

    for (const auto& boundary : boundaries) {
        FakeStorage storage;
        placeCurrent(storage, boundary);
        CrashJournalStore store(storage);
        const auto assessment = store.assessStartupAndEnterSafeMode();
        check(assessment.state == StartupRecoveryState::RecoverableIncomplete,
              "every valid nonterminal crash boundary is recoverable-incomplete");
        check(assessment.safeMode.has_value() &&
                  assessment.safeMode->reason == SafeModeReason::IncompleteSession &&
                  !assessment.safeModeWriteFailed,
              "incomplete boundary enters safe mode durably");
    }

    FakeStorage cleanStorage;
    placeCurrent(cleanStorage, cleanState(manifest));
    CrashJournalStore cleanStore(cleanStorage);
    const auto clean = cleanStore.assessStartupAndEnterSafeMode();
    check(clean.state == StartupRecoveryState::Clean && !clean.safeMode,
          "verified clean stop starts normally without safe mode");

    auto failedState = initialState(manifest);
    append(failedState, manifest, CrashJournalRecordKind::FailureRecorded, 0, 7);
    FakeStorage failedStorage;
    placeCurrent(failedStorage, failedState);
    CrashJournalStore failedStore(failedStorage);
    const auto failed = failedStore.assessStartupAndEnterSafeMode();
    check(failed.state == StartupRecoveryState::RecoveryRequired &&
              failed.safeMode.has_value() &&
              failed.safeMode->reason == SafeModeReason::RecoveryRequired,
          "explicit failure enters recovery-required safe mode");
}

void testCorruptionAndFutureVersionEnterSafeMode() {
    const auto manifest = samplePlan();
    auto bytes = encodeCrashJournal(initialState(manifest));
    check(!bytes.empty(), "corruption fixture encodes");

    FakeStorage corruptStorage;
    bytes.back() ^= std::byte{0x55};
    corruptStorage.files[JournalStorageSlot::Current] = bytes;
    CrashJournalStore corruptStore(corruptStorage);
    const auto corrupt = corruptStore.assessStartupAndEnterSafeMode();
    check(corrupt.state == StartupRecoveryState::RecoveryRequired &&
              corrupt.safeMode.has_value() &&
              corrupt.safeMode->reason == SafeModeReason::CorruptJournal,
          "checksum corruption enters recovery-required safe mode");

    FakeStorage futureStorage;
    bytes = encodeCrashJournal(initialState(manifest));
    bytes[4] = std::byte{2};
    futureStorage.files[JournalStorageSlot::Current] = bytes;
    CrashJournalStore futureStore(futureStorage);
    const auto future = futureStore.assessStartupAndEnterSafeMode();
    check(future.state == StartupRecoveryState::RecoveryRequired &&
              future.safeMode.has_value(),
          "future journal schema enters recovery-required safe mode");
}

void testWriteFailureBlocksRiskyActivation() {
    const auto manifest = samplePlan();
    const auto initial = initialState(manifest);
    FakeStorage storage;
    storage.failWriteOnce = JournalStorageSlot::CurrentTemp;
    CrashJournalStore store(storage);
    std::string error;
    check(!store.beginActivation(initial, &error),
          "critical journal write failure blocks activation");
    check(!storage.files.contains(JournalStorageSlot::Current),
          "failed activation does not fabricate current journal");
    const auto marker = store.loadSafeMode(&error);
    check(marker.has_value() &&
              marker->reason == SafeModeReason::JournalWriteFailure,
          "write failure attempts durable safe-mode marker");
}

void testAtomicReplaceFailurePreservesPriorState() {
    const auto manifest = samplePlan();
    const auto initial = initialState(manifest);
    std::string error;

    FakeStorage beginStorage;
    beginStorage.failReplaceOnce = std::pair{
        JournalStorageSlot::CurrentTemp, JournalStorageSlot::Current};
    CrashJournalStore beginStore(beginStorage);
    check(!beginStore.beginActivation(initial, &error),
          "atomic replace failure blocks initial activation");
    check(!beginStorage.files.contains(JournalStorageSlot::Current) &&
              !beginStorage.files.contains(JournalStorageSlot::CurrentTemp),
          "failed initial replace leaves no partial current/temp journal");
    const auto beginMarker = beginStore.loadSafeMode(&error);
    check(beginMarker.has_value() &&
              beginMarker->reason == SafeModeReason::JournalWriteFailure,
          "initial replace failure enters journal-write-failure safe mode");

    FakeStorage transitionStorage;
    CrashJournalStore transitionStore(transitionStorage);
    check(transitionStore.beginActivation(initial, &error),
          "transition replace fixture begins cleanly");
    const auto priorBytes = transitionStorage.files.at(JournalStorageSlot::Current);

    auto advanced = initial;
    append(advanced, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2);
    transitionStorage.failReplaceOnce = std::pair{
        JournalStorageSlot::CurrentTemp, JournalStorageSlot::Current};
    check(!transitionStore.persistTransition(advanced, &error),
          "atomic replace failure blocks a risky transition");
    check(transitionStorage.files.at(JournalStorageSlot::Current) == priorBytes &&
              !transitionStorage.files.contains(JournalStorageSlot::CurrentTemp),
          "failed transition replace preserves the last durable current journal");
    const auto transitionMarker = transitionStore.loadSafeMode(&error);
    check(transitionMarker.has_value() &&
              transitionMarker->reason == SafeModeReason::JournalWriteFailure,
          "transition replace failure enters journal-write-failure safe mode");
}

void testDurableTransitionCannotSkipBoundaries() {
    const auto manifest = samplePlan();
    const auto initial = initialState(manifest);
    FakeStorage storage;
    CrashJournalStore store(storage);
    std::string error;
    check(store.beginActivation(initial, &error),
          "durable-boundary fixture begins cleanly");
    const auto priorBytes = storage.files.at(JournalStorageSlot::Current);

    auto skipped = initial;
    append(skipped, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2);
    append(skipped, manifest, CrashJournalRecordKind::ActionApplied, 10, 2);
    check(!store.persistTransition(skipped, &error),
          "one durable write cannot skip multiple journal record boundaries");
    check(storage.files.at(JournalStorageSlot::Current) == priorBytes,
          "skipped-boundary rejection preserves the last durable journal");
    const auto marker = store.loadSafeMode(&error);
    check(marker.has_value() && marker->reason == SafeModeReason::RecoveryRequired,
          "skipped durable boundary enters recovery-required safe mode");
}

void testSafeModeBlocksRiskyActivation() {
    const auto manifest = samplePlan();
    const auto clean = cleanState(manifest, 7);
    FakeStorage storage;
    placeCurrent(storage, clean);
    CrashJournalStore store(storage);
    SafeModeMarker marker;
    marker.sessionId = clean.sessionId;
    marker.runtimeGeneration = clean.runtimeGeneration;
    marker.reason = SafeModeReason::ManualRecovery;
    marker.journalHash = hashCrashJournalBytes(encodeCrashJournal(clean));
    std::string error;
    check(store.writeSafeMode(marker, &error),
          "manual safe-mode fixture persists");

    const auto next = initialState(manifest, 8);
    const auto priorCurrent = storage.files.at(JournalStorageSlot::Current);
    check(!store.beginActivation(next, &error),
          "active safe mode blocks a new risky activation");
    check(storage.files.at(JournalStorageSlot::Current) == priorCurrent,
          "blocked activation cannot overwrite the last clean journal");
}

void testTransitionRewriteAndTruncationFailClosed() {
    const auto manifest = samplePlan();
    const auto initial = initialState(manifest);
    FakeStorage storage;
    CrashJournalStore store(storage);
    std::string error;
    check(store.beginActivation(initial, &error), "initial journal persists");

    auto rewritten = initial;
    rewritten.planHash = hash(0x90);
    check(!store.persistTransition(rewritten, &error),
          "immutable plan identity cannot be rewritten");
    auto marker = store.loadSafeMode(&error);
    check(marker.has_value() && marker->reason == SafeModeReason::RecoveryRequired,
          "rewrite attempt enters recovery-required safe mode");

    FakeStorage truncateStorage;
    CrashJournalStore truncateStore(truncateStorage);
    check(truncateStore.beginActivation(initial, &error),
          "truncate fixture initial persists");
    auto advanced = initial;
    append(advanced, manifest, CrashJournalRecordKind::ActionPrepared, 10, 2);
    check(truncateStore.persistTransition(advanced, &error),
          "advanced transition persists");
    check(!truncateStore.persistTransition(initial, &error),
          "record history cannot be truncated");
}

void testRotationIsBounded() {
    const auto manifest = samplePlan();
    FakeStorage storage;
    CrashJournalStore store(storage);
    std::string error;
    auto state = initialState(manifest);
    check(store.beginActivation(state, &error), "rotation fixture begins");

    const std::array<std::tuple<CrashJournalRecordKind, std::uint32_t,
                               std::uint64_t>, 6> transitions{{
        {CrashJournalRecordKind::ActionPrepared, 10, 2},
        {CrashJournalRecordKind::ActionApplied, 10, 2},
        {CrashJournalRecordKind::ActionVerified, 10, 2},
        {CrashJournalRecordKind::ActionPrepared, 20, 3},
        {CrashJournalRecordKind::ActionApplied, 20, 3},
        {CrashJournalRecordKind::ActionVerified, 20, 3},
    }};
    for (const auto& [kind, actionId, generation] : transitions) {
        append(state, manifest, kind, actionId, generation);
        check(store.persistTransition(state, &error),
              "rotation transition persists");
    }

    for (std::size_t index = 0; index < kCrashJournalHistoryDepth; ++index) {
        const auto slot = static_cast<JournalStorageSlot>(
            static_cast<std::uint16_t>(JournalStorageSlot::History0) +
            static_cast<std::uint16_t>(index));
        const auto found = storage.files.find(slot);
        check(found != storage.files.end() &&
                  decodeCrashJournal(found->second).has_value(),
              "bounded history slot contains valid prior journal");
    }
    check(storage.files.size() <= kCrashJournalHistoryDepth + 1,
          "journal rotation cannot grow unbounded when safe mode is absent");
}

void testVerifiedResetRequiredToClearSafeMode() {
    const auto manifest = samplePlan();
    const auto initial = initialState(manifest);
    FakeStorage storage;
    placeCurrent(storage, initial);
    CrashJournalStore store(storage);
    const auto assessment = store.assessStartupAndEnterSafeMode();
    check(assessment.state == StartupRecoveryState::RecoverableIncomplete &&
              assessment.safeMode.has_value(),
          "reset fixture starts in safe mode");

    std::string error;
    const auto clean = cleanState(manifest);
    check(!store.replaceWithVerifiedCleanState(clean, false, &error),
          "unverified reset cannot replace journal with clean state");
    check(store.replaceWithVerifiedCleanState(clean, true, &error),
          "verified reset may persist independently verified clean state");
    check(!store.clearSafeModeAfterVerifiedReset(
              clean.sessionId, clean.runtimeGeneration, false, &error),
          "safe mode cannot clear without verified reset flag");
    auto wrongSession = clean.sessionId;
    ++wrongSession[0];
    check(!store.clearSafeModeAfterVerifiedReset(
              wrongSession, clean.runtimeGeneration, true, &error),
          "safe mode cannot clear for a different session identity");
    check(store.clearSafeModeAfterVerifiedReset(
              clean.sessionId, clean.runtimeGeneration, true, &error),
          "verified matching clean reset clears safe mode");
    check(!store.loadSafeMode(), "safe-mode marker is gone after verified clear");
    check(store.assessStartupAndEnterSafeMode().state ==
              StartupRecoveryState::Clean,
          "startup is clean only after verified reset and marker clear");
}

void testJournalIsEvidenceNotInstructionChannel() {
    const auto manifest = samplePlan();
    const auto state = cleanState(manifest);
    const auto bytes = encodeCrashJournal(state);
    const std::string sentinel = "private-input-marker-XYZ";
    const auto begin = reinterpret_cast<const char*>(bytes.data());
    const std::string serialized(begin, begin + bytes.size());
    check(serialized.find(sentinel) == std::string::npos,
          "fixed-width journal contains no unrelated private fixture");

    auto mismatchedManifest = manifest;
    mismatchedManifest.actions[1].actionId = 21;
    std::string error;
    check(!validateCrashJournalAgainstPlan(state, mismatchedManifest, &error),
          "journal action ids cannot create recovery actions absent from trusted plan");
}

void testNativeStorageRoundTrip() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("hydraseat-crash-journal-test-" + std::to_string(nonce));
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);

    NativeCrashJournalStorage storage(root);
    CrashJournalStore store(storage);
    const auto manifest = samplePlan();
    std::string error;
    auto state = initialState(manifest);
    check(store.beginActivation(state, &error),
          "native storage durably begins activation");
    append(state, manifest, CrashJournalRecordKind::RollbackStarted, 0, 7);
    check(store.persistTransition(state, &error),
          "native storage durably persists rollback-start boundary");
    append(state, manifest, CrashJournalRecordKind::RollbackVerified, 0, 7);
    check(store.persistTransition(state, &error),
          "native storage durably persists rollback verification boundary");
    append(state, manifest, CrashJournalRecordKind::CleanStop, 0, 7);
    check(store.persistTransition(state, &error),
          "native storage atomically persists clean-stop boundary");
    check(store.loadCurrent(&error) == std::optional<CrashJournalState>(state),
          "native storage reads exact journal back");
    check(store.assessStartupAndEnterSafeMode().state == StartupRecoveryState::Clean,
          "native clean journal assesses clean");

    std::uint32_t defaultError = 0;
    const auto defaultDirectory = defaultCrashJournalDirectory(&defaultError);
    check(defaultDirectory.has_value() && !defaultDirectory->empty(),
          "platform default recovery directory resolves without writing to it");

    std::filesystem::remove_all(root, cleanupError);
    check(!cleanupError, "native storage test directory cleans up");
}

} // namespace

int main() {
    testCodecRoundTripAndCorruption();
    testSafeModeCodec();
    testCanonicalPlanHashBinding();
    testRecoveryProcessAttachmentJournalBinding();
    testStateMachineAndPlanBinding();
    testMaximumPlanFitsBoundedJournal();
    testStartupAssessmentAtTransitionBoundaries();
    testCorruptionAndFutureVersionEnterSafeMode();
    testWriteFailureBlocksRiskyActivation();
    testAtomicReplaceFailurePreservesPriorState();
    testDurableTransitionCannotSkipBoundaries();
    testSafeModeBlocksRiskyActivation();
    testTransitionRewriteAndTruncationFailClosed();
    testRotationIsBounded();
    testVerifiedResetRequiredToClearSafeMode();
    testJournalIsEvidenceNotInstructionChannel();
    testNativeStorageRoundTrip();

    std::cout << "Crash journal tests passed.\n";
    return EXIT_SUCCESS;
}
