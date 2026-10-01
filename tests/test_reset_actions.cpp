#include "hydra/reset_actions.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace hydra;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory {
public:
    explicit TempDirectory(std::string_view label) {
        static std::uint64_t counter = 0;
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        m_path = std::filesystem::temp_directory_path() /
            ("hydra-reset-" + std::string(label) + "-" +
             std::to_string(tick) + "-" + std::to_string(++counter));
        std::filesystem::create_directories(m_path);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

watchdog::SessionId sampleSession(std::uint8_t base = 1) {
    watchdog::SessionId result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(base + index);
    }
    return result;
}

watchdog::RollbackPlanManifest sampleManifest(
    watchdog::SessionId session = sampleSession()) {
    watchdog::RollbackPlanManifest manifest;
    manifest.lease.sessionId = session;
    manifest.lease.generation = 7;
    manifest.lease.timeoutMilliseconds = 2'000;
    manifest.rollbackTimeoutMilliseconds = 5'000;

    watchdog::RollbackActionDescriptor first;
    first.actionId = 10;
    first.kind = watchdog::RollbackActionKind::TerminateOwnedProcess;
    first.activationOrdinal = 1;
    first.timeoutMilliseconds = 500;
    first.generation = 7;
    first.process = {41010, 0x1000100010001000ull};

    watchdog::RollbackActionDescriptor second;
    second.actionId = 20;
    second.kind = watchdog::RollbackActionKind::ReleaseOverlayState;
    second.activationOrdinal = 2;
    second.timeoutMilliseconds = 500;
    second.generation = 7;
    second.resourceId = 0x2000200020002000ull;

    manifest.actions = {first, second};
    return manifest;
}

recovery::RecoveryProcessAttachmentIdentity attachmentIdentity(
    const watchdog::RollbackPlanManifest& manifest = sampleManifest()) {
    recovery::RecoveryProcessAttachmentIdentity identity;
    identity.seatId = 1u;
    for (std::size_t index = 0; index < identity.hostSessionId.bytes.size(); ++index) {
        identity.hostSessionId.bytes[index] =
            static_cast<std::uint8_t>(0xa0u + index + 1u);
    }
    identity.sessionGeneration = 11u;
    identity.seatGameGeneration = 13u;
    identity.process = manifest.actions.front().process;
    identity.recoveryEpoch = manifest.lease.generation;
    return identity;
}

reset::RuntimeResetRegistration sampleRegistration(
    const watchdog::RollbackPlanManifest& manifest = sampleManifest()) {
    reset::RuntimeResetRegistration registration;
    registration.ownerProcess = {40000, 0x9000900090009000ull};
    registration.manifest = manifest;
    registration.attachment = attachmentIdentity(manifest);
    return registration;
}

reset::RuntimeResetRegistration legacyRegistration(
    const watchdog::RollbackPlanManifest& manifest = sampleManifest()) {
    auto registration = sampleRegistration(manifest);
    registration.attachment.reset();
    return registration;
}

watchdog::RollbackPlanManifest attachmentManifest() {
    auto manifest = sampleManifest();
    manifest.actions.resize(1u);
    return manifest;
}

reset::RuntimeResetRegistration attachedRegistration() {
    return sampleRegistration(attachmentManifest());
}

void appendU16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffu));
    bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

std::vector<std::byte> legacyRegistrationBytes(
    const reset::RuntimeResetRegistration& registration) {
    const auto frame = watchdog::encodeRegisterPlan(1u, registration.manifest);
    std::vector<std::byte> hashInput;
    appendU32(hashInput, registration.ownerProcess.processId);
    appendU64(hashInput, registration.ownerProcess.creationTime100ns);
    hashInput.insert(hashInput.end(), frame.begin(), frame.end());
    const auto digest = recovery::hashCrashJournalBytes(hashInput);

    std::vector<std::byte> bytes;
    bytes.reserve(reset::kRuntimeResetRegistrationLegacyHeaderBytes + frame.size());
    appendU32(bytes, reset::kRuntimeResetRegistrationMagic);
    bytes.push_back(static_cast<std::byte>(
        reset::kRuntimeResetRegistrationLegacyVersion & 0xffu));
    bytes.push_back(static_cast<std::byte>(
        (reset::kRuntimeResetRegistrationLegacyVersion >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::byte>(
        reset::kRuntimeResetRegistrationLegacyHeaderBytes & 0xffu));
    bytes.push_back(static_cast<std::byte>(
        (reset::kRuntimeResetRegistrationLegacyHeaderBytes >> 8u) & 0xffu));
    appendU32(bytes, static_cast<std::uint32_t>(
        reset::kRuntimeResetRegistrationLegacyHeaderBytes + frame.size()));
    appendU32(bytes, registration.ownerProcess.processId);
    appendU64(bytes, registration.ownerProcess.creationTime100ns);
    for (const auto value : digest) bytes.push_back(static_cast<std::byte>(value));
    appendU32(bytes, static_cast<std::uint32_t>(frame.size()));
    appendU32(bytes, 0u);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    return bytes;
}

std::vector<std::byte> attachmentlessV2RegistrationBytes(
    const reset::RuntimeResetRegistration& registration) {
    const auto frame = watchdog::encodeRegisterPlan(1u, registration.manifest);
    std::vector<std::byte> hashInput;
    appendU32(hashInput, registration.ownerProcess.processId);
    appendU64(hashInput, registration.ownerProcess.creationTime100ns);
    appendU32(hashInput, 0u);
    hashInput.insert(hashInput.end(), frame.begin(), frame.end());
    const auto digest = recovery::hashCrashJournalBytes(hashInput);

    std::vector<std::byte> bytes;
    bytes.reserve(reset::kRuntimeResetRegistrationHeaderBytes + frame.size());
    appendU32(bytes, reset::kRuntimeResetRegistrationMagic);
    appendU16(bytes, reset::kRuntimeResetRegistrationVersion);
    appendU16(bytes, static_cast<std::uint16_t>(
        reset::kRuntimeResetRegistrationHeaderBytes));
    appendU32(bytes, static_cast<std::uint32_t>(
        reset::kRuntimeResetRegistrationHeaderBytes + frame.size()));
    appendU32(bytes, registration.ownerProcess.processId);
    appendU64(bytes, registration.ownerProcess.creationTime100ns);
    for (const auto value : digest) bytes.push_back(static_cast<std::byte>(value));
    appendU32(bytes, 0u);
    appendU32(bytes, static_cast<std::uint32_t>(frame.size()));
    appendU32(bytes, 0u);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    return bytes;
}

void writeRegistrationBytes(const std::filesystem::path& path,
                            const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    check(static_cast<bool>(output), "reset registration fixture bytes are written");
}

class FakeExecutor final : public watchdog::RollbackExecutor {
public:
    std::vector<std::uint32_t> calls;
    std::unordered_map<std::uint32_t, watchdog::RollbackActionResult> results;

    watchdog::RollbackActionOutcome terminateOwnedProcess(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    watchdog::RollbackActionOutcome closeOwnedSession(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    watchdog::RollbackActionOutcome clearOptionalBackendState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    watchdog::RollbackActionOutcome releaseOverlayState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    watchdog::RollbackActionOutcome restoreSnapshotState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }
    watchdog::RollbackActionOutcome writeSafeModeResult(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override {
        return run(action, timeoutMilliseconds);
    }

private:
    watchdog::RollbackActionOutcome run(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) {
        check(timeoutMilliseconds != 0, "reset action timeout remains bounded/nonzero");
        calls.push_back(action.actionId);
        const auto found = results.find(action.actionId);
        const auto result = found == results.end()
            ? watchdog::RollbackActionResult::Success
            : found->second;
        return {action.actionId, action.kind, result,
                result == watchdog::RollbackActionResult::Failed ? 55u : 0u};
    }
};

void persistRecord(recovery::CrashJournalStore& store,
                   recovery::CrashJournalState& state,
                   const watchdog::RollbackPlanManifest& manifest,
                   recovery::CrashJournalRecordKind kind,
                   std::uint32_t actionId,
                   std::uint64_t generation) {
    std::string error;
    check(recovery::appendCrashJournalRecord(
              state, manifest, kind, actionId, generation, &error),
          "journal record appends: " + error);
    check(store.persistTransition(state, &error),
          "journal transition persists: " + error);
}

recovery::CrashJournalState seedActiveJournal(
    recovery::CrashJournalStore& store,
    const watchdog::RollbackPlanManifest& manifest) {
    std::string error;
    const auto attachmentSnapshot =
        recovery::makeRecoveryProcessAttachmentSnapshot(
            attachmentIdentity(manifest), &error);
    check(attachmentSnapshot.has_value(),
          "initial reset test attachment snapshot is valid: " + error);
    const std::array snapshots{*attachmentSnapshot};
    auto state = recovery::makeInitialCrashJournal(
        manifest, manifest.lease.generation, snapshots, &error);
    check(state.has_value(), "initial reset test journal is valid: " + error);
    check(store.beginActivation(*state, &error),
          "reset test journal begins: " + error);
    for (const auto& action : manifest.actions) {
        persistRecord(store, *state, manifest,
                      recovery::CrashJournalRecordKind::ActionPrepared,
                      action.actionId, action.generation);
    }
    for (const auto& action : manifest.actions) {
        persistRecord(store, *state, manifest,
                      recovery::CrashJournalRecordKind::ActionApplied,
                      action.actionId, action.generation);
        persistRecord(store, *state, manifest,
                      recovery::CrashJournalRecordKind::ActionVerified,
                      action.actionId, action.generation);
    }
    persistRecord(store, *state, manifest,
                  recovery::CrashJournalRecordKind::ActivationCommitted,
                  0, manifest.lease.generation);
    check(state->phase == recovery::CrashJournalPhase::Active,
          "seeded reset test journal is active");
    return *state;
}

void testRegistrationCodecAndStorage() {
    const auto registration = sampleRegistration();
    std::string error;
    check(reset::validateRuntimeResetRegistration(registration, &error),
          "sample runtime reset registration validates");
    const auto encoded = reset::encodeRuntimeResetRegistration(registration);
    check(encoded.size() >= reset::kRuntimeResetRegistrationHeaderBytes &&
              encoded.size() <= reset::kRuntimeResetRegistrationMaxBytes,
          "runtime reset registration encoding is bounded");
    const auto decoded = reset::decodeRuntimeResetRegistration(encoded, &error);
    check(decoded && *decoded == registration && decoded->attachment,
          "runtime reset v2 registration round trips with exact attachment authority");

    const auto attached = attachedRegistration();
    check(reset::validateRuntimeResetRegistration(attached, &error),
          "single-process attachment-bound runtime reset registration validates");
    const auto attachedBytes = reset::encodeRuntimeResetRegistration(attached);
    check(attachedBytes.size() >=
              reset::kRuntimeResetRegistrationHeaderBytes +
                  recovery::kRecoveryProcessAttachmentIdentityBytes,
          "attachment-bound runtime reset registration persists full exact identity");
    const auto attachedDecoded =
        reset::decodeRuntimeResetRegistration(attachedBytes, &error);
    check(attachedDecoded && *attachedDecoded == attached,
          "attachment-bound runtime reset v2 registration round trips exactly");

    const auto legacy = legacyRegistration();
    const auto legacyBytes = legacyRegistrationBytes(legacy);
    const auto legacyDecoded =
        reset::decodeRuntimeResetRegistration(legacyBytes, &error);
    check(legacyDecoded && *legacyDecoded == legacy &&
              !legacyDecoded->attachment,
          "legacy runtime reset v1 registration remains explicitly readable");
    check(!reset::validateRuntimeResetRegistration(*legacyDecoded, &error),
          "decoded legacy registration is not current process-mutation authority");
    check(reset::encodeRuntimeResetRegistration(legacy).empty(),
          "current v2 writer refuses attachment-less downgrade authority");
    const auto attachmentlessV2 = attachmentlessV2RegistrationBytes(legacy);
    check(!reset::decodeRuntimeResetRegistration(attachmentlessV2, &error),
          "well-formed v2 registration without exact attachment authority is rejected");

    auto mismatchedAttachment = attached;
    ++mismatchedAttachment.attachment->process.creationTime100ns;
    check(!reset::validateRuntimeResetRegistration(mismatchedAttachment, &error),
          "reset attachment process identity must match its exact rollback action");

    auto badMagic = encoded;
    badMagic[0] = std::byte{0};
    check(!reset::decodeRuntimeResetRegistration(badMagic, &error),
          "runtime reset registration bad magic is rejected");

    auto futureVersion = encoded;
    futureVersion[4] = std::byte{3};
    futureVersion[5] = std::byte{0};
    check(!reset::decodeRuntimeResetRegistration(futureVersion, &error),
          "future runtime reset registration version is rejected");

    auto badReserved = encoded;
    badReserved[64] = std::byte{1};
    check(!reset::decodeRuntimeResetRegistration(badReserved, &error),
          "runtime reset registration nonzero reserved field is rejected");

    auto badHash = encoded;
    badHash[28] ^= std::byte{1};
    check(!reset::decodeRuntimeResetRegistration(badHash, &error),
          "runtime reset registration integrity-hash corruption is rejected");

    auto badOwnerIdentity = encoded;
    badOwnerIdentity[12] ^= std::byte{1};
    check(!reset::decodeRuntimeResetRegistration(badOwnerIdentity, &error),
          "runtime reset registration owner-identity corruption is rejected");

    auto truncated = encoded;
    truncated.pop_back();
    check(!reset::decodeRuntimeResetRegistration(truncated, &error),
          "runtime reset registration truncation is rejected");

    auto duplicateOwner = registration;
    duplicateOwner.ownerProcess = duplicateOwner.manifest.actions.front().process;
    check(!reset::validateRuntimeResetRegistration(duplicateOwner, &error),
          "runtime owner cannot duplicate a rollback target");

    auto reservedAction = registration;
    reservedAction.manifest.actions.front().actionId = reset::kResetOwnerActionId;
    check(!reset::validateRuntimeResetRegistration(reservedAction, &error),
          "runtime reset manifest cannot collide with the reserved owner action id");

    TempDirectory temp("codec");
    reset::RuntimeResetRegistrationStore store(temp.path());
    check(!store.write(legacy, &error),
          "runtime reset store refuses to persist legacy-shaped mutation authority");
    check(store.write(registration, &error),
          "runtime reset registration is durably written: " + error);
    const auto loaded = store.load();
    check(loaded.status == reset::RuntimeRegistrationReadStatus::Success &&
              loaded.registration && *loaded.registration == registration,
          "runtime reset registration storage round trips");
    check(store.remove(&error), "runtime reset registration removal succeeds");
    check(store.load().status == reset::RuntimeRegistrationReadStatus::Missing,
          "runtime reset registration removal is idempotently visible");
    check(store.remove(&error), "repeated runtime reset registration removal succeeds");
}

void testLegacyRegistrationIsDiagnosticOnly() {
    TempDirectory temp("legacy-diagnostic-only");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto legacy = legacyRegistration();
    writeRegistrationBytes(
        temp.path() / "reset-runtime.bin",
        legacyRegistrationBytes(legacy));

    const auto loaded = registrationStore.load();
    check(loaded.status == reset::RuntimeRegistrationReadStatus::Success &&
              loaded.registration && !loaded.registration->attachment,
          "legacy v1 registration remains readable as diagnostic evidence");
    const auto inspection = reset::inspectResetState(journalStore, registrationStore);
    check(inspection.state == reset::ResetState::RecoveryRequired,
          "pre-journal legacy registration is not promoted to recovery authority");

    FakeExecutor executor;
    const auto report = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(!report.success && executor.calls.empty(),
          "legacy registration cannot authorize any process/resource mutation");
    check(registrationStore.load().status ==
              reset::RuntimeRegistrationReadStatus::Success,
          "failed legacy authority check retains evidence for diagnosis/support");
}

void testPreJournalRegistrationRecovery() {
    TempDirectory temp("pre-journal");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto registration = sampleRegistration();
    std::string error;
    check(registrationStore.write(registration, &error),
          "pre-journal runtime registration writes");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Recoverable,
          "registration written before journal remains exactly recoverable");

    FakeExecutor executor;
    const auto report = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(report.success && report.journalClean &&
              report.registrationCleared &&
              executor.calls.size() == 3u &&
              executor.calls[0] == reset::kResetOwnerActionId &&
              executor.calls[1] == 20u && executor.calls[2] == 10u,
          "pre-journal activation crash window uses only registered exact actions");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Clean,
          "pre-journal reset removes its authority after verified cleanup");
}

void testPreJournalFailurePersistsRecoveryRequiredAndRetries() {
    TempDirectory temp("pre-journal-failure");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto registration = sampleRegistration();
    std::string error;
    check(registrationStore.write(registration, &error),
          "pre-journal failure registration writes");

    FakeExecutor executor;
    executor.results[20] = watchdog::RollbackActionResult::Failed;
    const auto failed = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(!failed.success && failed.ownerSatisfied && !failed.rollbackSatisfied,
          "pre-journal partial cleanup failure is not reported clean");
    const auto failureJournal = journalStore.loadCurrent(&error);
    check(failureJournal &&
              failureJournal->phase == recovery::CrashJournalPhase::Preparing,
          "pre-journal cleanup failure promotes registration into durable Preparing journal");
    check(failureJournal && registration.attachment &&
              recovery::validateRecoveryProcessAttachmentJournalBinding(
                  *failureJournal, *registration.attachment, &error),
          "pre-journal failure preserves exact attachment authority in durable journal evidence");
    const auto marker = journalStore.loadSafeMode(&error);
    check(marker && marker->reason == recovery::SafeModeReason::RecoveryRequired &&
              marker->sessionId == registration.manifest.lease.sessionId &&
              marker->runtimeGeneration == registration.manifest.lease.generation,
          "pre-journal cleanup failure records correlated RecoveryRequired safe mode");
    check(registrationStore.load().status ==
              reset::RuntimeRegistrationReadStatus::Success,
          "pre-journal failure retains trusted reset registration for retry");

    executor.results.erase(20);
    const auto retry = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(retry.success && retry.journalClean && retry.safeModeCleared &&
              retry.registrationCleared,
          "pre-journal RecoveryRequired state remains safely retryable");
}

void testAttachmentBoundJournalRejectsDowngradeAndMismatch() {
    TempDirectory temp("attachment-binding");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto manifest = attachmentManifest();
    const auto identity = attachmentIdentity(manifest);
    std::string error;
    const auto attachmentSnapshot =
        recovery::makeRecoveryProcessAttachmentSnapshot(identity, &error);
    check(attachmentSnapshot.has_value(),
          "attachment-bound reset fixture creates exact journal binding");
    const std::array snapshots{*attachmentSnapshot};
    const auto state = recovery::makeInitialCrashJournal(
        manifest, manifest.lease.generation, snapshots, &error);
    check(state && journalStore.beginActivation(*state, &error),
          "attachment-bound reset fixture persists its journal");

    auto exact = sampleRegistration(manifest);
    exact.attachment = identity;
    check(registrationStore.write(exact, &error),
          "attachment-bound reset registration persists");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Recoverable,
          "exact attachment registration and journal are recoverable");

    const auto downgraded = legacyRegistration(manifest);
    check(!registrationStore.write(downgraded, &error),
          "current writer refuses attachment-less downgrade authority");
    writeRegistrationBytes(
        temp.path() / "reset-runtime.bin",
        legacyRegistrationBytes(downgraded));
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::RecoveryRequired,
          "attachment-bound journal rejects readable legacy registration downgrade");

    auto mismatched = exact;
    ++mismatched.attachment->hostSessionId.bytes[0];
    check(registrationStore.write(mismatched, &error),
          "mismatched but individually valid attachment registration persists");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::RecoveryRequired,
          "journal rejects reset registration from another host attachment epoch");

    check(registrationStore.write(exact, &error),
          "exact attachment registration can be restored for verification");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Recoverable,
          "exact attachment registration restores correlated recoverability");
}

void testAttachmentPreJournalFailurePreservesExactBinding() {
    TempDirectory temp("attachment-pre-journal");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto registration = attachedRegistration();
    std::string error;
    check(registrationStore.write(registration, &error),
          "attachment pre-journal registration persists before risky activation");

    FakeExecutor executor;
    executor.results[registration.manifest.actions.front().actionId] =
        watchdog::RollbackActionResult::Failed;
    const auto failed = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(!failed.success && failed.ownerSatisfied && !failed.rollbackSatisfied,
          "attachment pre-journal rollback failure is not reported clean");
    const auto failureJournal = journalStore.loadCurrent(&error);
    check(failureJournal &&
              recovery::validateRecoveryProcessAttachmentJournalBinding(
                  *failureJournal, *registration.attachment, &error),
          "pre-journal failure promotes exact attachment identity into durable journal evidence");
    check(registrationStore.load().status ==
              reset::RuntimeRegistrationReadStatus::Success,
          "failed attachment recovery retains exact reset registration for retry");

    executor.results.clear();
    const auto retry = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(retry.success && retry.journalClean && retry.registrationCleared,
          "attachment-bound pre-journal recovery remains exactly retryable");
    const auto clean = journalStore.loadCurrent(&error);
    check(clean && recovery::validateRecoveryProcessAttachmentJournalBinding(
                       *clean, *registration.attachment, &error),
          "verified reset keeps exact attachment binding in terminal clean evidence");
}

void testNoSessionAndRepeatedReset() {
    TempDirectory temp("empty");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());

    const auto inspection = reset::inspectResetState(journalStore, registrationStore);
    check(inspection.state == reset::ResetState::Clean,
          "no active session inspects as clean");

    FakeExecutor executor;
    const auto first = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(first.success && first.noOp && executor.calls.empty(),
          "reset with no active session is a no-op");
    const auto second = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(second.success && second.noOp && executor.calls.empty(),
          "repeated empty reset remains a no-op");
}

void testActiveSessionResetAndCleanup() {
    TempDirectory temp("active");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto manifest = sampleManifest();
    (void)seedActiveJournal(journalStore, manifest);
    const auto registration = sampleRegistration(manifest);
    std::string error;
    check(registrationStore.write(registration, &error),
          "active reset registration is written");

    const auto assessment = journalStore.assessStartupAndEnterSafeMode();
    check(assessment.state == recovery::StartupRecoveryState::RecoverableIncomplete,
          "active journal startup assessment enters recoverable safe mode");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Recoverable,
          "active journal plus correlated registration is recoverable");

    FakeExecutor executor;
    const auto report = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(report.success && report.ownerSatisfied && report.rollbackSatisfied &&
              report.journalClean && report.safeModeCleared &&
              report.registrationCleared,
          "active emergency reset verifies every postcondition");
    check(executor.calls.size() == 3u &&
              executor.calls[0] == reset::kResetOwnerActionId &&
              executor.calls[1] == 20u && executor.calls[2] == 10u,
          "reset stops exact runtime owner then replays rollback in reverse activation order");
    const auto clean = journalStore.loadCurrent(&error);
    check(clean && clean->phase == recovery::CrashJournalPhase::Clean &&
              clean->finalResult == recovery::CrashJournalFinalResult::Clean,
          "verified reset persists canonical clean journal evidence");
    check(!journalStore.loadSafeMode(&error),
          "verified reset clears correlated safe mode");
    check(registrationStore.load().status ==
              reset::RuntimeRegistrationReadStatus::Missing,
          "verified reset removes runtime registration");

    const auto repeated = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(repeated.success && repeated.noOp && executor.calls.size() == 3u,
          "repeated reset after verified cleanup does not mutate processes again");
}

void testSessionMismatchAndRegistrationMismatchFailClosed() {
    {
        TempDirectory temp("session-mismatch");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        const auto manifest = sampleManifest();
        (void)seedActiveJournal(journalStore, manifest);
        std::string error;
        check(registrationStore.write(sampleRegistration(manifest), &error),
              "session mismatch registration writes");
        FakeExecutor executor;
        const auto report = reset::executeVerifiedReset(
            journalStore, registrationStore, executor, sampleSession(40));
        check(!report.success && executor.calls.empty(),
              "wrong requested session performs no reset action");
        const auto marker = journalStore.loadSafeMode(&error);
        check(marker && marker->reason == recovery::SafeModeReason::RecoveryRequired,
              "wrong requested session records RecoveryRequired");
    }

    {
        TempDirectory temp("plan-mismatch");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        const auto manifest = sampleManifest();
        (void)seedActiveJournal(journalStore, manifest);
        auto otherManifest = sampleManifest(sampleSession(70));
        std::string error;
        check(registrationStore.write(sampleRegistration(otherManifest), &error),
              "mismatched registration is individually valid");
        const auto inspection =
            reset::inspectResetState(journalStore, registrationStore);
        check(inspection.state == reset::ResetState::RecoveryRequired,
              "registration/journal session mismatch fails closed");
        FakeExecutor executor;
        const auto report = reset::executeVerifiedReset(
            journalStore, registrationStore, executor);
        check(!report.success && executor.calls.empty(),
              "mismatched registration executes no process action");
    }
}

void testMissingCorruptAndPartialFailureStayRecoverableOrRequired() {
    {
        TempDirectory temp("prejournal-safe-mode-conflict");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        std::string error;
        check(registrationStore.write(sampleRegistration(), &error),
              "pre-journal conflict registration writes");
        recovery::SafeModeMarker marker;
        marker.reason = recovery::SafeModeReason::RecoveryRequired;
        marker.diagnosticCode = 91;
        check(journalStore.writeSafeMode(marker, &error),
              "pre-journal conflict safe mode writes");
        FakeExecutor executor;
        const auto report = reset::executeVerifiedReset(
            journalStore, registrationStore, executor);
        check(!report.success && executor.calls.empty() &&
                  report.before.state == reset::ResetState::RecoveryRequired,
              "safe mode without journal blocks pre-journal registration execution");
    }

    {
        TempDirectory temp("missing-registration");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        (void)seedActiveJournal(journalStore, sampleManifest());
        check(reset::inspectResetState(journalStore, registrationStore).state ==
                  reset::ResetState::RecoveryRequired,
              "incomplete journal without recovery authority fails closed");
        FakeExecutor executor;
        const auto report = reset::executeVerifiedReset(
            journalStore, registrationStore, executor);
        check(!report.success && executor.calls.empty(),
              "missing registration cannot trigger broad cleanup");
    }

    {
        TempDirectory temp("corrupt-registration");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        (void)seedActiveJournal(journalStore, sampleManifest());
        std::ofstream output(temp.path() / "reset-runtime.bin", std::ios::binary);
        output << "not-a-valid-registration";
        output.close();
        check(reset::inspectResetState(journalStore, registrationStore).state ==
                  reset::ResetState::RecoveryRequired,
              "corrupt recovery authority fails closed");
    }

    {
        TempDirectory temp("partial-failure");
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        reset::RuntimeResetRegistrationStore registrationStore(temp.path());
        const auto manifest = sampleManifest();
        (void)seedActiveJournal(journalStore, manifest);
        std::string error;
        check(registrationStore.write(sampleRegistration(manifest), &error),
              "partial-failure registration writes");
        FakeExecutor executor;
        executor.results[20] = watchdog::RollbackActionResult::Failed;
        const auto report = reset::executeVerifiedReset(
            journalStore, registrationStore, executor);
        check(!report.success && report.ownerSatisfied &&
                  !report.rollbackSatisfied,
              "partial rollback failure is not reported clean");
        check(registrationStore.load().status ==
                  reset::RuntimeRegistrationReadStatus::Success,
              "partial failure retains trusted registration for retry/support");
        const auto marker = journalStore.loadSafeMode(&error);
        check(marker && marker->reason == recovery::SafeModeReason::RecoveryRequired,
              "partial failure records RecoveryRequired safe mode");

        executor.results.erase(20);
        const auto retry = reset::executeVerifiedReset(
            journalStore, registrationStore, executor);
        check(retry.success,
              "repeated reset can recover after a transient partial failure");
    }
}

void testCleanJournalClearsOnlyStaleMetadata() {
    TempDirectory temp("stale-metadata");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    const auto manifest = sampleManifest();
    (void)seedActiveJournal(journalStore, manifest);
    std::string error;
    check(registrationStore.write(sampleRegistration(manifest), &error),
          "clean-stale setup registration writes");
    FakeExecutor initialExecutor;
    check(reset::executeVerifiedReset(
              journalStore, registrationStore, initialExecutor).success,
          "clean-stale setup reset succeeds");

    check(registrationStore.write(sampleRegistration(manifest), &error),
          "stale registration can be seeded for cleanup test");
    recovery::SafeModeMarker marker;
    marker.sessionId = manifest.lease.sessionId;
    marker.runtimeGeneration = manifest.lease.generation;
    marker.reason = recovery::SafeModeReason::ManualRecovery;
    const auto current = journalStore.loadCurrent(&error);
    check(current.has_value(), "clean journal reloads for stale marker");
    marker.journalHash = recovery::hashCrashJournalBytes(
        recovery::encodeCrashJournal(*current));
    check(journalStore.writeSafeMode(marker, &error),
          "correlated stale safe-mode marker writes");

    FakeExecutor shouldNotRun;
    shouldNotRun.results[reset::kResetOwnerActionId] =
        watchdog::RollbackActionResult::Failed;
    const auto report = reset::executeVerifiedReset(
        journalStore, registrationStore, shouldNotRun);
    check(report.success && shouldNotRun.calls.empty(),
          "clean journal clears stale metadata without terminating any process");
    check(reset::inspectResetState(journalStore, registrationStore).state ==
              reset::ResetState::Clean,
          "stale clean metadata cleanup ends clean");
}

void testManualSafeModeAndSessionParsing() {
    TempDirectory temp("manual-safe-mode");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    std::string error;

    check(reset::enableManualSafeMode(journalStore, &error),
          "manual safe mode enables without a session");
    const auto marker = journalStore.loadSafeMode(&error);
    check(marker && marker->reason == recovery::SafeModeReason::ManualRecovery &&
              watchdog::isZeroSessionId(marker->sessionId) &&
              marker->runtimeGeneration == 0,
          "manual no-session safe mode uses zero recovery identity");
    check(reset::disableManualSafeMode(storage, journalStore, &error),
          "manual no-session safe mode can be disabled explicitly");
    check(!journalStore.loadSafeMode(&error),
          "manual safe mode marker is removed");

    recovery::SafeModeMarker forcedMarker;
    forcedMarker.reason = recovery::SafeModeReason::RecoveryRequired;
    forcedMarker.diagnosticCode = 77;
    check(journalStore.writeSafeMode(forcedMarker, &error),
          "non-manual safe-mode marker can be seeded for overwrite protection");
    check(!reset::enableManualSafeMode(journalStore, &error),
          "manual safe mode cannot overwrite an existing RecoveryRequired marker");
    const auto retainedMarker = journalStore.loadSafeMode(&error);
    check(retainedMarker && retainedMarker->reason ==
              recovery::SafeModeReason::RecoveryRequired,
          "existing RecoveryRequired marker is preserved exactly");
    check(!reset::disableManualSafeMode(storage, journalStore, &error),
          "non-manual zero-identity safe mode cannot be cleared as manual mode");
    std::uint32_t removeError = 0;
    check(storage.remove(recovery::JournalStorageSlot::SafeMode, &removeError),
          "test removes protected marker before continuing");

    const auto manifest = sampleManifest();
    (void)seedActiveJournal(journalStore, manifest);
    check(reset::enableManualSafeMode(journalStore, &error),
          "manual safe mode can bind to an active journal identity");
    check(!reset::disableManualSafeMode(storage, journalStore, &error),
          "session-bound active safe mode cannot be cleared without verified reset");

    const auto text = reset::sessionIdHex(manifest.lease.sessionId);
    const auto parsed = reset::parseSessionIdHex(text, &error);
    check(parsed && *parsed == manifest.lease.sessionId,
          "session id hexadecimal format round trips");
    check(!reset::parseSessionIdHex("1234", &error),
          "short session id is rejected");
    check(!reset::parseSessionIdHex(
              "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", &error),
          "non-hex session id is rejected");
    check(!reset::parseSessionIdHex(
              "00000000000000000000000000000000", &error),
          "zero session id is rejected");

    (void)registrationStore;
}

void testUnsupportedRollbackResultFailsClosed() {
    TempDirectory temp("unsupported");
    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    auto manifest = sampleManifest();
    manifest.actions[1].kind = watchdog::RollbackActionKind::ReleaseOverlayState;
    manifest.actions[1].process = {};
    manifest.actions[1].resourceId = 123;
    std::string error;
    check(watchdog::validateRollbackPlan(manifest, &error),
          "typed unsupported reset action remains a valid watchdog manifest");
    (void)seedActiveJournal(journalStore, manifest);
    check(registrationStore.write(sampleRegistration(manifest), &error),
          "unsupported-action registration writes");
    FakeExecutor executor;
    executor.results[20] = watchdog::RollbackActionResult::Unsupported;
    const auto report = reset::executeVerifiedReset(
        journalStore, registrationStore, executor);
    check(!report.success && !report.rollbackSatisfied &&
              report.rollback.recoveryRequired,
          "unsupported reset action fails closed as RecoveryRequired");
}

} // namespace

int main() {
    testRegistrationCodecAndStorage();
    testLegacyRegistrationIsDiagnosticOnly();
    testPreJournalRegistrationRecovery();
    testPreJournalFailurePersistsRecoveryRequiredAndRetries();
    testAttachmentBoundJournalRejectsDowngradeAndMismatch();
    testAttachmentPreJournalFailurePreservesExactBinding();
    testNoSessionAndRepeatedReset();
    testActiveSessionResetAndCleanup();
    testSessionMismatchAndRegistrationMismatchFailClosed();
    testMissingCorruptAndPartialFailureStayRecoverableOrRequired();
    testCleanJournalClearsOnlyStaleMetadata();
    testManualSafeModeAndSessionParsing();
    testUnsupportedRollbackResultFailsClosed();
    std::cout << "Reset action tests passed.\n";
    return EXIT_SUCCESS;
}
